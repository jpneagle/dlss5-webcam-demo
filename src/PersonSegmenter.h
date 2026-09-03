#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Person alpha matte from Robust Video Matting (mobilenetv3), run on the CPU through
// ONNX Runtime.
//
// RVM is a video model: it carries four recurrent states between frames, so the matte
// is temporally stable without any smoothing of our own. That is exactly what a mask
// feeding a temporal neural renderer needs -- a per-frame segmentation would flicker
// the boundary and drag the reconstruction with it.
//
// Inference lives on its own thread. The render loop never waits for it; it uses the
// newest matte available, so a slow model degrades mask latency rather than frame rate.
class PersonSegmenter {
public:
    // Both are defined in the .cpp: the pimpl is incomplete here, so the compiler
    // cannot generate them (it needs the member destructor for exception cleanup).
    PersonSegmenter();
    ~PersonSegmenter();

    // netW/netH is the resolution the matte is produced at. downsampleRatio is RVM's
    // own internal scale: the network runs at ratio*size and the matte is refined back
    // up, which is far cheaper than running the whole network at full size.
    bool Initialize(const std::wstring& modelPath, uint32_t netW, uint32_t netH,
                    float downsampleRatio, int threads);
    void Shutdown();

    bool Available() const { return m_running.load(std::memory_order_acquire); }

    // Hands the newest camera frame to the worker. Non-blocking: if inference is still
    // busy the frame is dropped, which is the correct behaviour for a live source.
    void Submit(const uint8_t* bgra, uint32_t width, uint32_t height);

    // Returns the newest matte (8-bit, netW x netH) if one arrived since the last call.
    bool TryGetMask(std::vector<uint8_t>& out);

    uint32_t MaskWidth() const { return m_netW; }
    uint32_t MaskHeight() const { return m_netH; }
    double LastInferenceMs() const { return m_lastMs.load(std::memory_order_relaxed); }
    uint64_t Inferences() const { return m_inferences.load(std::memory_order_relaxed); }
    const std::wstring& StatusText() const { return m_status; }

private:
    struct Impl;                      // keeps the ONNX Runtime headers out of this one
    std::unique_ptr<Impl> m_impl;

    void Worker();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    std::mutex m_inputMutex;
    std::vector<uint8_t> m_pendingBgra;   // one slot; a newer frame overwrites an unread one
    uint32_t m_pendingW = 0, m_pendingH = 0;
    bool m_pendingFilled = false;

    std::mutex m_maskMutex;
    std::vector<uint8_t> m_mask;
    bool m_maskFilled = false;

    uint32_t m_netW = 0, m_netH = 0;
    float m_downsampleRatio = 0.5f;
    std::atomic<double> m_lastMs{0.0};
    std::atomic<uint64_t> m_inferences{0};
    std::wstring m_status = L"off";
};
