#include "DLSSNRBackend.h"
#include "Log.h"
#include <filesystem>
#include <sstream>

namespace {

// Feature 18 is NVSDK_NGX_Feature_Reserved18 in the public header. The DLSS 5
// Neural Rendering snippet answers on that slot.
constexpr NVSDK_NGX_Feature kFeatureDLSSNR = NVSDK_NGX_Feature_Reserved18;

// Parameter names observed in community implementations. Not a published contract.
constexpr const char* kWidth = "DLSSNR.Width";
constexpr const char* kHeight = "DLSSNR.Height";
constexpr const char* kInputWidth = "DLSSNR.InputWidth";
constexpr const char* kInputHeight = "DLSSNR.InputHeight";
constexpr const char* kOutputWidth = "DLSSNR.OutputWidth";
constexpr const char* kOutputHeight = "DLSSNR.OutputHeight";
constexpr const char* kOutputWidth2 = "DLSSNR.Output.Width";
constexpr const char* kOutputHeight2 = "DLSSNR.Output.Height";
constexpr const char* kUpscaling = "DLSSNR.Upscaling";
constexpr const char* kScale = "DLSSNR.Scale";
constexpr const char* kScalingRatio = "DLSSNR.ScalingRatio";

constexpr const char* kColor = "DLSSNR.Color";
constexpr const char* kOutput = "DLSSNR.Output";
constexpr const char* kMVec = "DLSSNR.MVec";
constexpr const char* kDepth = "DLSSNR.Depth";
constexpr const char* kMVecScaleX = "DLSSNR.MVecScaleX";
constexpr const char* kMVecScaleY = "DLSSNR.MVecScaleY";
constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
constexpr const char* kEnabled = "DLSSNR.Enabled";
constexpr const char* kReset = "DLSSNR.Reset";
constexpr const char* kStyle = "DLSSNR.Style";
constexpr const char* kIntensity = "DLSSNR.Intensity";
constexpr const char* kLocalTone = "DLSSNR.LocalToneStrength";
constexpr const char* kLocalStructure = "DLSSNR.LocalStructureStrength";
constexpr const char* kUseAutoMask = "DLSSNR.UseAutoMask";
constexpr const char* kSkinStructure = "DLSSNR.SkinStructureStrength";
// Subrect spellings confirmed by inspecting renodx-dlss5.addon64: no dot before
// "Subrect", unlike the Output.Width / Output.Height create-time aliases.
constexpr const char* kColorSubrectW = "DLSSNR.ColorSubrectWidth";
constexpr const char* kColorSubrectH = "DLSSNR.ColorSubrectHeight";
constexpr const char* kOutputSubrectW = "DLSSNR.OutputSubrectWidth";
constexpr const char* kOutputSubrectH = "DLSSNR.OutputSubrectHeight";
constexpr const char* kMVecSubrectW = "DLSSNR.MVecSubrectWidth";
constexpr const char* kMVecSubrectH = "DLSSNR.MVecSubrectHeight";
constexpr const char* kDepthSubrectW = "DLSSNR.DepthSubrectWidth";
constexpr const char* kDepthSubrectH = "DLSSNR.DepthSubrectHeight";

std::filesystem::path ExecutableDir() {
    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(exe).parent_path();
}

} // namespace

DLSSNRBackend::~DLSSNRBackend() {
    Shutdown();
}

bool DLSSNRBackend::LoadRuntime() {
    const std::filesystem::path dll = ExecutableDir() / L"nvngx_dlssnr.dll";
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        m_status = L"nvngx_dlssnr.dll not staged; NR runs only through ReShade/RenoDX";
        LOG("DLSSNR: no runtime beside the executable. Neural Rendering will only be "
            "available if ReShade/RenoDX injects it.");
        return false;
    }

    m_module = LoadLibraryW(dll.c_str());
    if (!m_module) {
        m_status = L"runtime failed to load";
        LOG("DLSSNR: LoadLibrary failed winerr=" << GetLastError());
        return false;
    }

    auto get = [&](const char* name) { return GetProcAddress(m_module, name); };
    m_init = reinterpret_cast<PFN_Init_Ext>(get("NVSDK_NGX_D3D12_Init_Ext"));
    m_createFeature = reinterpret_cast<PFN_CreateFeature>(get("NVSDK_NGX_D3D12_CreateFeature"));
    m_evaluateFeature = reinterpret_cast<PFN_EvaluateFeature>(get("NVSDK_NGX_D3D12_EvaluateFeature"));
    m_releaseFeature = reinterpret_cast<PFN_ReleaseFeature>(get("NVSDK_NGX_D3D12_ReleaseFeature"));
    m_shutdown = reinterpret_cast<PFN_Shutdown1>(get("NVSDK_NGX_D3D12_Shutdown1"));

    if (!m_init || !m_createFeature || !m_evaluateFeature || !m_releaseFeature) {
        m_status = L"runtime is missing the D3D12 entry points";
        LOG("DLSSNR: runtime does not export the expected D3D12 entry points.");
        FreeLibrary(m_module);
        m_module = nullptr;
        return false;
    }

    uint64_t bytes = 0;
    bytes = uint64_t(std::filesystem::file_size(dll, ec));
    unsigned int snippet = 0;
    if (auto ver = reinterpret_cast<PFN_GetSnippetVersion>(get("NVSDK_NGX_GetSnippetVersion"))) snippet = ver();
    LOG("DLSSNR: runtime loaded, " << bytes << " bytes, snippet version 0x" << std::hex << snippet << std::dec);
    return true;
}

bool DLSSNRBackend::Initialize(ID3D12Device* device, uint32_t width, uint32_t height) {
    Shutdown();
    m_device = device;
    m_width = width;
    m_height = height;

    if (!LoadRuntime()) return false;

    const std::filesystem::path logDir = ExecutableDir() / L"ngx_logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    // The parameter block comes from the regular NGX core, which the DLSS super
    // sampling backend has already initialised. The snippet only reads and writes
    // through the interface, so a core-owned block is what community hosts use.
    m_lastResult = NVSDK_NGX_D3D12_AllocateParameters(&m_params);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_params) {
        m_status = L"parameter allocation failed";
        LOG("DLSSNR: AllocateParameters failed result=0x" << std::hex << m_lastResult << std::dec);
        Shutdown();
        return false;
    }

    // The snippet is normally initialised by the NGX core, which passes it context
    // this call cannot reproduce. There is no published contract for calling it
    // directly, so probe the identity/version combinations community hosts use and
    // report exactly which one the staged runtime accepts.
    struct Attempt { unsigned long long appId; unsigned int version; bool withParams; const char* label; };
    const Attempt attempts[] = {
        { 0x4E52'4443ull, NVSDK_NGX_VERSION_API_MACRO, false, "customAppId api1.5" },
        { 0x4E52'4443ull, NVSDK_NGX_VERSION_API_MACRO, true,  "customAppId api1.5 +params" },
        { 0ull,           NVSDK_NGX_VERSION_API_MACRO, false, "appId0 api1.5" },
        { 1ull,           NVSDK_NGX_VERSION_API_MACRO, false, "appId1 api1.5" },
        { 0x4E52'4443ull, 0x0000016u,                  false, "customAppId api1.6" },
        { 0x4E52'4443ull, 0x0000017u,                  false, "customAppId api1.7" },
        { 0x4E52'4443ull, 0x0000018u,                  false, "customAppId api1.8" },
        { 0x4E52'4443ull, 0x0000020u,                  false, "customAppId api2.0" },
    };

    m_lastResult = NVSDK_NGX_Result_Fail;
    std::ostringstream probeLog;
    for (const Attempt& a : attempts) {
        const NVSDK_NGX_Result r = m_init(a.appId, logDir.c_str(), device,
                                          NVSDK_NGX_Version(a.version),
                                          a.withParams ? m_params : nullptr);
        probeLog << " [" << a.label << "]=0x" << std::hex << r << std::dec;
        m_lastResult = r;
        if (!NVSDK_NGX_FAILED(r)) break;
    }
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        m_status = L"direct init refused (0xbad00002 platform error)";
        LOG("DLSSNR: the staged runtime refused direct initialisation:" << probeLog.str());
        LOG("DLSSNR: 0xbad00002 is FAIL_PlatformError. The snippet expects to be driven by "
            "the NGX core rather than called directly; Neural Rendering therefore runs only "
            "through ReShade/RenoDX injection on this runtime.");
        Shutdown();
        return false;
    }
    LOG("DLSSNR: direct initialisation accepted:" << probeLog.str());
    m_initialized = true;

    m_available = true;
    m_status = L"runtime ready";
    LOG("DLSSNR: initialized for " << width << "x" << height << "; feature creation is deferred to a command list.");
    return true;
}

void DLSSNRBackend::FillCreateParameters() {
    NVSDK_NGX_Parameter_SetUI(m_params, kWidth, m_width);
    NVSDK_NGX_Parameter_SetUI(m_params, kHeight, m_height);
    NVSDK_NGX_Parameter_SetUI(m_params, kInputWidth, m_width);
    NVSDK_NGX_Parameter_SetUI(m_params, kInputHeight, m_height);
    NVSDK_NGX_Parameter_SetUI(m_params, kOutputWidth, m_width);
    NVSDK_NGX_Parameter_SetUI(m_params, kOutputHeight, m_height);
    NVSDK_NGX_Parameter_SetUI(m_params, kOutputWidth2, m_width);
    NVSDK_NGX_Parameter_SetUI(m_params, kOutputHeight2, m_height);
    // Neural Rendering only; the super-resolution work is already done by DLSS SR.
    NVSDK_NGX_Parameter_SetUI(m_params, kUpscaling, 0);
    NVSDK_NGX_Parameter_SetF(m_params, kScale, 1.0f);
    NVSDK_NGX_Parameter_SetF(m_params, kScalingRatio, 1.0f);
}

bool DLSSNRBackend::EnsureFeature(ID3D12GraphicsCommandList* cmd) {
    if (!m_available || !cmd) return false;
    if (m_handle) return true;
    if (m_createAttempted) return false;   // do not retry a failing create every frame
    m_createAttempted = true;

    FillCreateParameters();
    m_lastResult = m_createFeature(cmd, kFeatureDLSSNR, m_params, &m_handle);
    if (NVSDK_NGX_FAILED(m_lastResult) || !m_handle) {
        m_handle = nullptr;
        m_available = false;
        m_status = L"CreateFeature(18) failed";
        LOG("DLSSNR: CreateFeature(feature=18) FAILED result=0x" << std::hex << m_lastResult << std::dec
            << " at " << m_width << "x" << m_height);
        return false;
    }
    m_status = L"feature 18 created";
    LOG("DLSSNR: CreateFeature(feature=18) SUCCESS at " << m_width << "x" << m_height);
    return true;
}

bool DLSSNRBackend::Evaluate(ID3D12GraphicsCommandList* cmd,
                             ID3D12Resource* color,
                             ID3D12Resource* output,
                             ID3D12Resource* motion,
                             ID3D12Resource* depth,
                             bool reset,
                             const Settings& settings) {
    if (!m_handle || !cmd || !color || !output) return false;

    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, kColor, color);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, kOutput, output);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, kMVec, motion);
    NVSDK_NGX_Parameter_SetD3d12Resource(m_params, kDepth, depth);
    // Motion vectors are already expressed in pixels of the NR working resolution.
    NVSDK_NGX_Parameter_SetF(m_params, kMVecScaleX, 1.0f);
    NVSDK_NGX_Parameter_SetF(m_params, kMVecScaleY, 1.0f);
    // The depth proxy is a normalised inverse depth: near = 1, far = 0.
    NVSDK_NGX_Parameter_SetUI(m_params, kDepthInverted, 1);
    NVSDK_NGX_Parameter_SetUI(m_params, kEnabled, 1);
    NVSDK_NGX_Parameter_SetUI(m_params, kReset, reset ? 1 : 0);
    NVSDK_NGX_Parameter_SetUI(m_params, kStyle, uint32_t(settings.style));
    NVSDK_NGX_Parameter_SetF(m_params, kIntensity, settings.intensity);
    NVSDK_NGX_Parameter_SetF(m_params, kLocalTone, settings.localTone);
    NVSDK_NGX_Parameter_SetF(m_params, kLocalStructure, settings.localStructure);
    NVSDK_NGX_Parameter_SetUI(m_params, kUseAutoMask, settings.autoMask ? 1 : 0);

    m_lastResult = m_evaluateFeature(cmd, m_handle, m_params, nullptr);
    if (NVSDK_NGX_FAILED(m_lastResult)) {
        m_status = L"EvaluateFeature failed";
        if (m_evaluations == 0 || (m_evaluations % 300) == 0)
            LOG("DLSSNR: EvaluateFeature FAILED result=0x" << std::hex << m_lastResult << std::dec);
        return false;
    }
    ++m_evaluations;
    if (m_evaluations == 1 || (m_evaluations % 300) == 0)
        LOG("DLSSNR: EvaluateFeature SUCCESS #" << m_evaluations << " " << m_width << "x" << m_height
            << " style=" << uint32_t(settings.style)
            << " intensity=" << settings.intensity
            << " tone=" << settings.localTone
            << " structure=" << settings.localStructure
            << " autoMask=" << (settings.autoMask ? 1 : 0)
            << " reset=" << (reset ? 1 : 0));
    return true;
}

void DLSSNRBackend::Shutdown() {
    if (m_handle && m_releaseFeature) m_releaseFeature(m_handle);
    m_handle = nullptr;
    if (m_params) {
        NVSDK_NGX_D3D12_DestroyParameters(m_params);
        m_params = nullptr;
    }
    if (m_initialized && m_shutdown && m_device) m_shutdown(m_device);
    m_initialized = false;
    if (m_module) {
        FreeLibrary(m_module);
        m_module = nullptr;
    }
    m_init = nullptr;
    m_createFeature = nullptr;
    m_evaluateFeature = nullptr;
    m_releaseFeature = nullptr;
    m_shutdown = nullptr;
    m_available = false;
    m_createAttempted = false;
    m_evaluations = 0;
    m_device = nullptr;
}
