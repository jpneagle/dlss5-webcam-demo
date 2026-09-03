#include "CameraSource.h"
#include "Log.h"
#include <mferror.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

struct MediaBufferLock {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer2> buffer2d;
    BYTE* data = nullptr;
    LONG stride = 0;
    bool locked2d = false;

    ~MediaBufferLock() {
        if (locked2d && buffer2d) buffer2d->Unlock2D();
        else if (data && buffer) buffer->Unlock();
    }
};

const char* SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_ARGB32) return "ARGB32";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_YV12) return "YV12";
    return "other";
}

// Uncompressed formats avoid a decode step; MJPG is usually the only way a USB
// webcam reaches 30 fps at 720p or above, so it is preferred over a slow YUY2 mode.
int SubtypeRank(const GUID& g) {
    if (g == MFVideoFormat_NV12) return 3;
    if (g == MFVideoFormat_MJPG) return 2;
    if (g == MFVideoFormat_YUY2) return 1;
    return 0;
}

double FrameRateOf(IMFMediaType* type) {
    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) || !den) return 0.0;
    return double(num) / double(den);
}

} // namespace

CameraSource::CameraSource() {
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

CameraSource::~CameraSource() {
    Close();
    if (m_frameEvent) CloseHandle(m_frameEvent);
}

std::vector<CameraDevice> CameraSource::Enumerate() {
    std::vector<CameraDevice> devices;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return devices;
    if (FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return devices;

    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &found, &count)) || !found) return devices;

    for (UINT32 i = 0; i < count; ++i) {
        CameraDevice dev;
        WCHAR* text = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(found[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &text, &len)) && text) {
            dev.name.assign(text, len);
            CoTaskMemFree(text);
        }
        text = nullptr; len = 0;
        if (SUCCEEDED(found[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &text, &len)) && text) {
            dev.symbolicLink.assign(text, len);
            CoTaskMemFree(text);
        }
        if (!dev.symbolicLink.empty()) {
            if (dev.name.empty()) dev.name = L"Camera " + std::to_wstring(i);
            devices.push_back(std::move(dev));
        }
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return devices;
}

bool CameraSource::Open(const CameraDevice& device, uint32_t requestedW, uint32_t requestedH, double requestedFps) {
    Close();

    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, device.symbolicLink.c_str());

    HRESULT hr = MFCreateDeviceSource(attrs.Get(), &m_source);
    if (FAILED(hr) || !m_source) {
        LOG("Camera: MFCreateDeviceSource failed hr=0x" << std::hex << hr);
        return false;
    }

    ComPtr<IMFAttributes> readerAttrs;
    if (FAILED(MFCreateAttributes(&readerAttrs, 2))) { Close(); return false; }
    // Lets the reader insert the MJPEG decoder and the color converter that produce RGB32.
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);

    hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), readerAttrs.Get(), &m_reader);
    if (FAILED(hr) || !m_reader) {
        LOG("Camera: MFCreateSourceReaderFromMediaSource failed hr=0x" << std::hex << hr);
        Close();
        return false;
    }

    if (!ConfigureStream(requestedW, requestedH, requestedFps)) {
        Close();
        return false;
    }

    m_deviceName = device.name;
    m_stop.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&CameraSource::CaptureLoop, this);
    return true;
}

bool CameraSource::ConfigureStream(uint32_t requestedW, uint32_t requestedH, double requestedFps) {
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // Score every native mode: resolution distance first, then frame rate, then
    // subtype. A demo needs the requested size at full rate far more than it needs
    // the camera's maximum resolution.
    ComPtr<IMFMediaType> best;
    double bestScore = -1e18;
    UINT32 bestW = 0, bestH = 0;
    double bestFps = 0.0;
    GUID bestSubtype{};

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = m_reader->GetNativeMediaType(stream, i, &type);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !type) break;

        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) continue;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        const double fps = FrameRateOf(type.Get());

        const double sizeErr = std::abs(std::log(double(w) / double(std::max(1u, requestedW))))
                             + std::abs(std::log(double(h) / double(std::max(1u, requestedH))));
        const double fpsShortfall = std::max(0.0, requestedFps - fps);
        const double score = -sizeErr * 100.0 - fpsShortfall * 2.0 + SubtypeRank(subtype) * 0.5;

        if (score > bestScore) {
            bestScore = score;
            best = type;
            bestW = w; bestH = h; bestFps = fps; bestSubtype = subtype;
        }
    }

    if (!best) {
        LOG("Camera: no usable native media type.");
        return false;
    }
    LOG("Camera: selected native mode " << bestW << "x" << bestH << " @" << bestFps
        << " subtype=" << SubtypeName(bestSubtype));

    HRESULT hr = m_reader->SetCurrentMediaType(stream, nullptr, best.Get());
    if (FAILED(hr)) {
        LOG("Camera: SetCurrentMediaType(native) failed hr=0x" << std::hex << hr);
        return false;
    }

    // Ask the reader for BGRA. MF calls this RGB32; in memory it is B,G,R,A which is
    // exactly what D3D12Renderer uploads.
    ComPtr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(&outType))) return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, bestW, bestH);
    // Positive stride requests top-down delivery; MF may still hand back bottom-up.
    outType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(bestW * 4));

    hr = m_reader->SetCurrentMediaType(stream, nullptr, outType.Get());
    if (FAILED(hr)) {
        LOG("Camera: SetCurrentMediaType(RGB32) failed hr=0x" << std::hex << hr
            << "; the device format cannot be converted to RGB32.");
        return false;
    }

    ComPtr<IMFMediaType> actual;
    if (SUCCEEDED(m_reader->GetCurrentMediaType(stream, &actual)) && actual) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h))) { bestW = w; bestH = h; }
        const double fps = FrameRateOf(actual.Get());
        if (fps > 0.0) bestFps = fps;
        UINT32 declaredStride = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &declaredStride)))
            m_stride = int32_t(declaredStride);
    }

    m_width = bestW;
    m_height = bestH;
    m_fps = bestFps > 1.0 ? bestFps : 30.0;
    {
        const char* sub = SubtypeName(bestSubtype);
        m_formatName.assign(sub, sub + strlen(sub));
    }
    if (m_stride == 0) m_stride = int32_t(m_width * 4);

    LOG("Camera: output RGB32 " << m_width << "x" << m_height << " @" << m_fps
        << " stride=" << m_stride);
    return true;
}

void CameraSource::CaptureLoop() {
    // The capture thread lives in MTA so the source reader's worker callbacks and
    // the MJPEG decoder do not marshal back to the UI apartment.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // A USB webcam hands back transient failures: another process grabbing the
    // device, a hub power blip, a resume from sleep. Dropping the stream on the
    // first one would leave the demo with a dead window, so failures are retried
    // and only a sustained run of them gives up.
    int consecutiveFailures = 0;
    constexpr int kMaxConsecutiveFailures = 120;   // ~1.2 s of retries

    while (!m_stop.load(std::memory_order_acquire)) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                          &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            if (consecutiveFailures == 0)
                LOG("Camera: ReadSample failed hr=0x" << std::hex << hr << std::dec << "; retrying.");
            if (++consecutiveFailures >= kMaxConsecutiveFailures) {
                LOG("Camera: giving up after " << consecutiveFailures
                    << " consecutive ReadSample failures (last hr=0x" << std::hex << hr << std::dec
                    << "). The host will try to reopen the device.");
                break;
            }
            Sleep(10);
            continue;
        }
        consecutiveFailures = 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            LOG("Camera: end of stream (device removed or stopped).");
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            LOG("Camera: media type changed mid-stream; reconfiguring is not supported, stopping.");
            break;
        }
        if (!sample) continue;   // a null sample with no flags is a normal timeout tick
        PublishSample(sample.Get());
    }

    m_running.store(false, std::memory_order_release);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

void CameraSource::PublishSample(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return;

    MediaBufferLock lock;
    lock.buffer = buffer;
    BYTE* base = nullptr;
    LONG stride = m_stride;

    if (SUCCEEDED(buffer.As(&lock.buffer2d)) && lock.buffer2d) {
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        BYTE* bufferStart = nullptr;
        DWORD bufferLength = 0;
        if (SUCCEEDED(lock.buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch,
                                                &bufferStart, &bufferLength))) {
            lock.locked2d = true;
            lock.data = scanline0;
            base = scanline0;
            stride = pitch;
        }
    }
    if (!base) {
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&base, &maxLen, &curLen)) || !base) return;
        lock.data = base;
    }

    const uint32_t rowBytes = m_width * 4;
    const size_t needed = size_t(rowBytes) * m_height;

    VideoFrame frame;
    frame.bgra.resize(needed);

    // A negative stride means the buffer starts at the bottom row. Copy row by row so
    // the renderer always receives top-down BGRA.
    if (stride < 0) {
        const BYTE* src = base;
        for (uint32_t y = 0; y < m_height; ++y)
            memcpy(frame.bgra.data() + size_t(y) * rowBytes, src + ptrdiff_t(stride) * ptrdiff_t(y), rowBytes);
    } else {
        for (uint32_t y = 0; y < m_height; ++y)
            memcpy(frame.bgra.data() + size_t(y) * rowBytes, base + size_t(stride) * y, rowBytes);
    }

    LONGLONG sampleTime = 0;
    sample->GetSampleTime(&sampleTime);
    frame.timestamp100ns = int64_t(sampleTime);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    frame.captureQpc = qpc.QuadPart;

    m_captured.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(m_slotMutex);
        if (m_slotFilled) m_dropped.fetch_add(1, std::memory_order_relaxed);
        m_slot = std::move(frame);
        m_slotFilled = true;
    }
    if (m_frameEvent) SetEvent(m_frameEvent);
}

bool CameraSource::TryGetLatest(VideoFrame& out) {
    std::lock_guard<std::mutex> guard(m_slotMutex);
    if (!m_slotFilled) return false;
    out = std::move(m_slot);
    m_slot = VideoFrame{};
    m_slotFilled = false;
    return true;
}

void CameraSource::Close() {
    m_stop.store(true, std::memory_order_release);
    if (m_source) m_source->Stop();   // unblocks a ReadSample already in flight
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_release);

    m_reader.Reset();
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    {
        std::lock_guard<std::mutex> guard(m_slotMutex);
        m_slot = VideoFrame{};
        m_slotFilled = false;
    }
    m_width = m_height = 0;
    m_stride = 0;
    m_deviceName.clear();
    m_captured.store(0, std::memory_order_relaxed);
    m_dropped.store(0, std::memory_order_relaxed);
}
