#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>
#include <vector>
#include "DLSSBackend.h"
#include "DLSSNRBackend.h"

class D3D12Renderer {
public:
    enum class DebugView { Final, Input, MotionVectors, Depth, BiasMask, PersonMask };

    struct ColorSettings {
        float brightness = 0.0f;   // exposure-like brightness, in stops (-2..+2)
        float contrast = 1.0f;     // 0..3
        float saturation = 1.0f;   // 0..3
        float gamma = 1.0f;        // 0.25..3
        float temperature = 0.0f;  // -1..+1 (cool..warm)
        float tint = 0.0f;         // -1..+1 (green..magenta)
    };

    ~D3D12Renderer();
    bool Initialize(HWND hwnd, uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    uint32_t gridW, uint32_t gridH,
                    NVSDK_NGX_PerfQuality_Value quality,
                    bool tryDirectNR = false);
    bool RenderFrame(const uint8_t* bgra, size_t bytes,
                     const float* guideGridRGBA32F, size_t guideBytes,
                     uint32_t gridW, uint32_t gridH,
                     bool temporalReset, float frameTimeMs);

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    bool DLSSEnabled() const { return m_dlssEnabled && m_dlss.Available(); }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    void SetDebugView(DebugView v) { m_debugView = v; }
    DebugView GetDebugView() const { return m_debugView; }
    void RequestDLSSRecreate() { m_recreateRequested = true; }
    uint64_t FramesPresented() const { return m_framesPresented; }
    bool DLSSFeatureCreated() const { return m_dlss.FeatureCreated(); }
    bool NRAvailable() const { return m_nr.Available(); }
    bool NRFeatureCreated() const { return m_nr.FeatureCreated(); }
    uint64_t NREvaluations() const { return m_nr.EvaluationCount(); }
    const std::wstring& NRStatus() const { return m_nr.StatusText(); }
    uint64_t DLSSEvaluations() const { return m_dlss.EvaluationCount(); }
    bool DLSSLastEvaluationUsedC() const { return m_dlss.LastEvaluationUsedC(); }
    NVSDK_NGX_Result DLSSLastResult() const { return m_dlss.LastResult(); }
    // Writes the DLSS input and the reconstructed output as two PNGs. Still-image
    // A/B is the only way to judge whether a neural runtime hooked into NGX improved
    // the picture or just reinterpreted it.
    bool SaveComparisonPNG(const std::wstring& directory, std::wstring& outMessage);
    void WaitGPU();
    bool PresentCurrent();
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; }
    const ColorSettings& GetColorSettings() const { return m_colorSettings; }

    // How the untouched input is shown against the reconstruction. Wipe overlays the
    // two on one image and is best for spotting a difference; SideBySide shows the
    // whole frame twice and is the only mode where the same feature can be compared
    // before and after at the same moment.
    enum class CompareMode { Off, Wipe, SideBySide };
    void SetCompareMode(CompareMode m) { m_compare = m; }
    CompareMode GetCompareMode() const { return m_compare; }
    void SetSplitPosition(float t);
    float SplitPosition() const { return m_splitPos; }
    void SetMirror(bool v) { m_mirror = v; }
    bool Mirror() const { return m_mirror; }

    // Region where the neural reconstruction is allowed to replace the original.
    // Phase 1 of the mask work: a fixed feathered oval, before any segmentation.
    struct MaskSettings {
        bool enabled = false;          // fixed oval
        bool segmentation = false;     // RVM person matte
        float centerX = 0.5f, centerY = 0.5f;
        float radiusX = 0.26f, radiusY = 0.42f;
        float feather = 0.35f;
        float strength = 1.0f;
    };
    // Per-channel gain for the reconstructed image. The neural passes leave a
    // measurable colour cast; correcting it here -- after DLSS, in the presentation
    // pass -- fixes the picture without touching anything the neural renderer sees,
    // so temporal history is unaffected.
    struct NeuralGain { float r = 1.0f, g = 1.0f, b = 1.0f; };
    void SetNeuralGain(const NeuralGain& g) { m_neuralGain = g; }
    const NeuralGain& GetNeuralGain() const { return m_neuralGain; }

    void SetMask(const MaskSettings& m) { m_mask = m; }
    const MaskSettings& GetMask() const { return m_mask; }

    // Hands the newest person matte to the renderer. Created on first use, so the
    // segmenter decides the matte resolution.
    bool UpdatePersonMask(const uint8_t* r8, uint32_t width, uint32_t height);

private:
    static constexpr uint32_t FrameCount = 3;
    // NVIDIA's D3D12 DLSS contract expects input resources in NON_PIXEL_SHADER_RESOURCE
    // at EvaluateFeature time. Debug/presentation passes temporarily transition selected
    // resources to PIXEL_SHADER_RESOURCE and restore them before the frame ends.
    static constexpr D3D12_RESOURCE_STATES GuideReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES DepthGuideReadState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateHeapsAndBackbuffers();
    bool CreatePipelines();
    bool CreateVideoResources();
    bool EnsurePersonMaskResources(uint32_t width, uint32_t height);
    bool InitializeDLSS();
    bool m_tryDirectNR = false;
    bool CreateUploadForTexture(const D3D12_RESOURCE_DESC& desc,
                                Microsoft::WRL::ComPtr<ID3D12Resource>& upload,
                                uint8_t*& mapped,
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                uint32_t& rows,
                                uint64_t& rowBytes,
                                uint64_t& totalBytes,
                                const char* name);
    void CopyMappedRows(uint8_t* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        const void* src, size_t tightRowBytes, uint32_t rows);
    void RecordPresentPass(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool dlssUsed);
    bool WaitForFrameSlot(uint32_t slot);
    void SignalFrameSlot(uint32_t slot);
    void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE SRVCPU(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SRVGPU(uint32_t index) const;
    static float Halton(uint32_t index, uint32_t base);

    HWND m_hwnd = nullptr;
    uint32_t m_sourceW=0,m_sourceH=0,m_outputW=0,m_outputH=0,m_renderW=0,m_renderH=0,m_gridW=0,m_gridH=0;
    NVSDK_NGX_PerfQuality_Value m_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmds[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint64_t m_frameFence[FrameCount]{};
    uint32_t m_frameSlot = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoComposite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMaskPreview;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_biasCurrent;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_personMask;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_personMaskUpload[FrameCount];
    uint8_t* m_personMaskMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_personMaskFootprint{};
    uint32_t m_personMaskW = 0, m_personMaskH = 0, m_personMaskRows = 0;
    uint64_t m_personMaskRowSize = 0, m_personMaskBytes = 0;
    std::vector<uint8_t> m_pendingPersonMask;
    bool m_personMaskDirty = false;
    bool m_personMaskInCopyDest = true;

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    uint32_t m_numRows=0,m_guideRows=0;
    uint64_t m_rowSize=0,m_uploadBytes=0,m_guideRowSize=0,m_guideUploadBytes=0;

    bool m_sourceInCopyDest = true;
    bool m_gridInCopyDest = true;
    bool m_colorInRT = true;
    bool m_guidesInRT = true;
    bool m_depthInWrite = true;
    bool m_outputInUAV = true;
    bool m_dlssEnabled = true;
    bool m_allowTearing = false;
    bool m_recreateRequested = false;
    uint64_t m_framesPresented = 0;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    bool m_lastDLSSUsed = false;
    CompareMode m_compare = CompareMode::Wipe;
    bool m_mirror = true;
    float m_splitPos = 0.5f;
    MaskSettings m_mask{};
    NeuralGain m_neuralGain{};
    DLSSBackend m_dlss;
    DLSSNRBackend m_nr;
};
