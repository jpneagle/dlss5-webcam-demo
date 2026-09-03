#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One decoded webcam frame in BGRA8, matching the layout D3D12Renderer expects.
struct VideoFrame {
    std::vector<uint8_t> bgra;
    int64_t timestamp100ns = 0;   // device sample time
    int64_t captureQpc = 0;       // QueryPerformanceCounter at delivery, for latency measurement
    bool discontinuity = false;
};

struct CameraDevice {
    std::wstring name;
    std::wstring symbolicLink;
};

// Live webcam capture through Media Foundation.
//
// Capture runs on its own thread and publishes into a single-slot mailbox. A demo
// that falls behind must show the newest frame, not a backlog, so an undelivered
// frame is overwritten rather than queued. Dropped frames are counted so the
// caller can reset temporal history when the gap is large.
class CameraSource {
public:
    CameraSource();
    ~CameraSource();

    static std::vector<CameraDevice> Enumerate();

    bool Open(const CameraDevice& device, uint32_t requestedW, uint32_t requestedH, double requestedFps);
    void Close();

    // Moves the newest captured frame into `out`. Returns false when no new frame
    // has arrived since the last call.
    bool TryGetLatest(VideoFrame& out);

    // Auto-reset event, signalled whenever a frame is published. Lets the main loop
    // wait on the camera and on window messages together instead of polling.
    HANDLE FrameEvent() const { return m_frameEvent; }

    bool IsOpen() const { return m_running.load(std::memory_order_acquire); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    double FrameRate() const { return m_fps; }
    double DisplayAspectRatio() const { return m_height ? double(m_width) / double(m_height) : 16.0 / 9.0; }
    const std::wstring& DeviceName() const { return m_deviceName; }
    const wchar_t* FormatName() const { return m_formatName.c_str(); }
    uint64_t DroppedFrames() const { return m_dropped.load(std::memory_order_relaxed); }
    uint64_t CapturedFrames() const { return m_captured.load(std::memory_order_relaxed); }

private:
    bool ConfigureStream(uint32_t requestedW, uint32_t requestedH, double requestedFps);
    void CaptureLoop();
    void PublishSample(IMFSample* sample);

    Microsoft::WRL::ComPtr<IMFMediaSource> m_source;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    HANDLE m_frameEvent = nullptr;
    std::mutex m_slotMutex;
    VideoFrame m_slot;
    bool m_slotFilled = false;

    std::atomic<uint64_t> m_captured{0};
    std::atomic<uint64_t> m_dropped{0};

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int32_t m_stride = 0;          // negative for bottom-up delivery
    double m_fps = 30.0;
    std::wstring m_deviceName;
    std::wstring m_formatName = L"?";
};
