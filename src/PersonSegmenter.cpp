#include "PersonSegmenter.h"
#include "Log.h"
#include <onnxruntime_cxx_api.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace {

// One recurrent state, kept as plain floats between frames so nothing depends on an
// Ort::Value staying alive across two Run calls.
struct RecurrentState {
    std::vector<int64_t> shape{1, 1, 1, 1};
    std::vector<float> data{0.0f};

    void Reset() {
        shape = {1, 1, 1, 1};
        data.assign(1, 0.0f);
    }
    void Adopt(const Ort::Value& v) {
        const auto info = v.GetTensorTypeAndShapeInfo();
        shape = info.GetShape();
        const size_t count = size_t(info.GetElementCount());
        data.resize(count);
        std::memcpy(data.data(), v.GetTensorData<float>(), count * sizeof(float));
    }
};

} // namespace

struct PersonSegmenter::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "DLSSCamDemo"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<float> src;           // NCHW, RGB, [0,1]
    RecurrentState r[4];
    float ratio = 0.5f;

    // The model was inspected before writing this: src/r1i..r4i/downsample_ratio in,
    // fgr/pha/r1o..r4o out. Only pha is used.
    const char* inputNames[6] = {"src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"};
    const char* outputNames[6] = {"fgr", "pha", "r1o", "r2o", "r3o", "r4o"};
};

PersonSegmenter::PersonSegmenter() = default;

PersonSegmenter::~PersonSegmenter() {
    Shutdown();
}

bool PersonSegmenter::Initialize(const std::wstring& modelPath, uint32_t netW, uint32_t netH,
                                 float downsampleRatio, int threads) {
    Shutdown();

    std::error_code ec;
    if (!std::filesystem::exists(modelPath, ec)) {
        m_status = L"model not found";
        LOG("Segmenter: model not found at " << Narrow(modelPath)
            << "; the person mask falls back to the fixed oval.");
        return false;
    }

    m_netW = netW;
    m_netH = netH;
    m_downsampleRatio = downsampleRatio;
    m_impl = std::make_unique<Impl>();

    try {
        m_impl->options.SetIntraOpNumThreads(threads);
        m_impl->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_impl->session = std::make_unique<Ort::Session>(m_impl->env, modelPath.c_str(), m_impl->options);
    } catch (const Ort::Exception& e) {
        m_status = L"ONNX Runtime failed to load the model";
        LOG("Segmenter: ONNX Runtime error: " << e.what());
        m_impl.reset();
        return false;
    }

    m_impl->ratio = downsampleRatio;
    m_impl->src.assign(size_t(netW) * netH * 3, 0.0f);
    for (auto& state : m_impl->r) state.Reset();
    m_mask.assign(size_t(netW) * netH, 0);

    m_stop.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&PersonSegmenter::Worker, this);
    m_status = L"running";
    LOG("Segmenter: RVM ready at " << netW << "x" << netH
        << " downsample_ratio=" << downsampleRatio << " threads=" << threads);
    return true;
}

void PersonSegmenter::Submit(const uint8_t* bgra, uint32_t width, uint32_t height) {
    if (!bgra || !m_running.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> guard(m_inputMutex);
    const size_t bytes = size_t(width) * height * 4;
    m_pendingBgra.resize(bytes);
    std::memcpy(m_pendingBgra.data(), bgra, bytes);
    m_pendingW = width;
    m_pendingH = height;
    m_pendingFilled = true;
}

bool PersonSegmenter::TryGetMask(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> guard(m_maskMutex);
    if (!m_maskFilled) return false;
    out = m_mask;
    m_maskFilled = false;
    return true;
}

void PersonSegmenter::Worker() {
    std::vector<uint8_t> frame;
    uint32_t frameW = 0, frameH = 0;

    while (!m_stop.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> guard(m_inputMutex);
            if (m_pendingFilled) {
                frame.swap(m_pendingBgra);
                frameW = m_pendingW;
                frameH = m_pendingH;
                m_pendingFilled = false;
            } else {
                frameW = 0;
            }
        }
        if (!frameW || !frameH) {
            Sleep(2);
            continue;
        }

        const auto t0 = std::chrono::steady_clock::now();

        // BGRA -> planar RGB float in [0,1], point-sampled down to the network size.
        // A box filter would be marginally cleaner but this runs every frame and the
        // matte is refined by the model's own guided filter anyway.
        const size_t plane = size_t(m_netW) * m_netH;
        float* dstR = m_impl->src.data();
        float* dstG = dstR + plane;
        float* dstB = dstG + plane;
        for (uint32_t y = 0; y < m_netH; ++y) {
            const uint32_t sy = uint32_t((uint64_t(y) * frameH) / m_netH);
            const uint8_t* srcRow = frame.data() + size_t(sy) * frameW * 4;
            for (uint32_t x = 0; x < m_netW; ++x) {
                const uint32_t sx = uint32_t((uint64_t(x) * frameW) / m_netW);
                const uint8_t* p = srcRow + size_t(sx) * 4;
                const size_t o = size_t(y) * m_netW + x;
                dstB[o] = float(p[0]) * (1.0f / 255.0f);
                dstG[o] = float(p[1]) * (1.0f / 255.0f);
                dstR[o] = float(p[2]) * (1.0f / 255.0f);
            }
        }

        try {
            const int64_t srcShape[4] = {1, 3, int64_t(m_netH), int64_t(m_netW)};
            const int64_t ratioShape[1] = {1};

            std::vector<Ort::Value> inputs;
            inputs.reserve(6);
            inputs.push_back(Ort::Value::CreateTensor<float>(m_impl->memory, m_impl->src.data(),
                                                             m_impl->src.size(), srcShape, 4));
            for (auto& state : m_impl->r)
                inputs.push_back(Ort::Value::CreateTensor<float>(m_impl->memory, state.data.data(),
                                                                 state.data.size(),
                                                                 state.shape.data(), state.shape.size()));
            inputs.push_back(Ort::Value::CreateTensor<float>(m_impl->memory, &m_impl->ratio, 1,
                                                             ratioShape, 1));

            auto outputs = m_impl->session->Run(Ort::RunOptions{nullptr},
                                                m_impl->inputNames, inputs.data(), inputs.size(),
                                                m_impl->outputNames, 6);

            const float* pha = outputs[1].GetTensorData<float>();
            {
                std::lock_guard<std::mutex> guard(m_maskMutex);
                m_mask.resize(plane);
                for (size_t i = 0; i < plane; ++i)
                    m_mask[i] = uint8_t(std::lround(std::clamp(pha[i], 0.0f, 1.0f) * 255.0f));
                m_maskFilled = true;
            }
            // Carry the recurrent states into the next frame; this is what makes the
            // matte stable instead of flickering per frame.
            for (int i = 0; i < 4; ++i) m_impl->r[i].Adopt(outputs[2 + i]);

            m_inferences.fetch_add(1, std::memory_order_relaxed);
        } catch (const Ort::Exception& e) {
            LOG("Segmenter: inference failed: " << e.what());
            for (auto& state : m_impl->r) state.Reset();
            Sleep(200);
        }

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_lastMs.store(ms, std::memory_order_relaxed);
    }
    m_running.store(false, std::memory_order_release);
}

void PersonSegmenter::Shutdown() {
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_release);
    m_impl.reset();
    {
        std::lock_guard<std::mutex> guard(m_maskMutex);
        m_mask.clear();
        m_maskFilled = false;
    }
    {
        std::lock_guard<std::mutex> guard(m_inputMutex);
        m_pendingBgra.clear();
        m_pendingFilled = false;
    }
    m_inferences.store(0, std::memory_order_relaxed);
    m_lastMs.store(0.0, std::memory_order_relaxed);
    m_status = L"off";
}
