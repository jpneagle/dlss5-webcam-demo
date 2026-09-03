#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>
#include <string>

// Direct host for the experimental DLSS 5 Neural Rendering runtime (NGX feature 18).
//
// This is NOT a documented NVIDIA API. The feature id, the parameter names and the
// resource formats below come from community analysis of nvngx_dlssnr.dll, not from
// a published SDK, and a runtime update can change any of them. The alternative path
// -- letting ReShade/RenoDX hook the DLSS super-sampling evaluate -- stays available
// and is what runs when this backend reports unavailable.
//
// The runtime is never shipped with this demo. The user stages their own copy as
// nvngx_dlssnr.dll beside the executable.
class DLSSNRBackend {
public:
    // Style is the runtime's model selection. Community builds expose three.
    enum class Style : uint32_t { Default = 0, Natural = 1, Cinematic = 2 };

    struct Settings {
        Style style = Style::Natural;
        float intensity = 0.5f;
        float localTone = 0.5f;
        float localStructure = 0.5f;
        bool autoMask = true;
    };

    ~DLSSNRBackend();

    // Loads the runtime and initialises it. Returns false (quietly) when the user has
    // not staged a runtime; that is the normal case, not an error.
    bool Initialize(ID3D12Device* device, uint32_t width, uint32_t height);
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd);
    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* color,
                  ID3D12Resource* output,
                  ID3D12Resource* motion,
                  ID3D12Resource* depth,
                  bool reset,
                  const Settings& settings);
    void Shutdown();

    bool Available() const { return m_available; }
    bool FeatureCreated() const { return m_handle != nullptr; }
    uint64_t EvaluationCount() const { return m_evaluations; }
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }
    const std::wstring& StatusText() const { return m_status; }
    void SetUnavailableReason(const wchar_t* text) { m_status = text; }

private:
    bool LoadRuntime();
    void FillCreateParameters();

    using PFN_Init_Ext = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                                       NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using PFN_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                            NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using PFN_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                              const NVSDK_NGX_Parameter*, void*);
    using PFN_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
    using PFN_Shutdown1 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
    using PFN_GetSnippetVersion = unsigned int(NVSDK_CONV*)();

    HMODULE m_module = nullptr;
    PFN_Init_Ext m_init = nullptr;
    PFN_CreateFeature m_createFeature = nullptr;
    PFN_EvaluateFeature m_evaluateFeature = nullptr;
    PFN_ReleaseFeature m_releaseFeature = nullptr;
    PFN_Shutdown1 m_shutdown = nullptr;

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    uint32_t m_width = 0, m_height = 0;
    bool m_available = false;
    bool m_initialized = false;
    bool m_createAttempted = false;
    uint64_t m_evaluations = 0;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    std::wstring m_status = L"not loaded";
};
