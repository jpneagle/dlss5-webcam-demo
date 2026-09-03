// DLSSCamDemo - webcam -> temporal guides -> D3D12 -> NGX DLSS (-> RenoDX DLSS 5 NR)
//
// Derived from the DLSS Video Player in ../dlss5video/player. The DLSS/NGX contract,
// the temporal guide generation and the D3D12 renderer are unchanged; only the input
// source (file -> live camera) and the pacing model (audio clock -> capture driven)
// differ.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "CameraSource.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "PersonSegmenter.h"
#include "Log.h"

#pragma comment(lib, "comctl32.lib")

using Clock = std::chrono::steady_clock;

static constexpr int CONTROL_H = 64;

// Overlay hotkeys keep working while ReShade captures normal keyboard input.
static constexpr int HK_DLSS = 9101;
static constexpr int HK_RECREATE = 9102;
static constexpr int HK_MIRROR = 9103;
static constexpr int HK_CAPTURE = 9104;
static constexpr int HK_MASK = 9105;
static constexpr int HK_SEGMENT = 9106;
static constexpr int HK_CAPTURE_MODE = 9107;
static constexpr int HK_WARMER = 9108;
static constexpr int HK_COOLER = 9109;

static constexpr int IDM_CAMERA_BASE = 5000;
static constexpr int IDM_VIEW_FINAL = 5100;
static constexpr int IDM_VIEW_INPUT = 5101;
static constexpr int IDM_VIEW_MV = 5102;
static constexpr int IDM_VIEW_DEPTH = 5103;
static constexpr int IDM_VIEW_MASK = 5104;
static constexpr int IDM_VIEW_SPLIT = 5105;
static constexpr int IDM_VIEW_SIDEBYSIDE = 5110;
static constexpr int IDM_VIEW_MIRROR = 5106;
static constexpr int IDM_VIEW_MASKPREVIEW = 5107;
static constexpr int IDM_MASK_TOGGLE = 5108;
static constexpr int IDM_MASK_SEGMENT = 5109;
static constexpr int IDM_CAPTURE_BASE = 5300;
static constexpr int IDM_NEUTRAL_RESET = 5400;
static constexpr int IDM_DLSS_TOGGLE = 5200;
static constexpr int IDM_DLSS_RECREATE = 5201;
static constexpr int IDM_EXIT = 5900;

// Capture resolutions offered at runtime. The low ones are the point of the demo:
// DLSS 5 Neural Rendering only has room to show what it does when there is a real gap
// between input and output. A native 720p webcam frame is already photographic and
// leaves the model almost nothing to reconstruct.
struct CaptureMode {
    uint32_t width, height;
    const wchar_t* label;
};
static const CaptureMode kCaptureModes[] = {
    {1280, 720, L"1280x720  native"},
    { 960, 540, L"960x540"},
    { 640, 360, L"640x360  reconstruction demo"},
    { 320, 180, L"320x180  extreme"},
};

struct AppOptions {
    int deviceIndex = -1;              // -1 = ask / use saved
    uint32_t captureW = 1280;
    uint32_t captureH = 720;
    double captureFps = 30.0;
    uint32_t outW = 2560;
    uint32_t outH = 1440;
    NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    bool captureExplicit = false;
    bool tryDirectNR = false;   // --nr-direct: probe the unsupported direct feature-18 path
};

static AppOptions ParseArgs() {
    AppOptions o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    auto num = [&](int i) { return (i < argc) ? _wtoi(argv[i]) : 0; };
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--device" && i + 1 < argc) o.deviceIndex = num(++i);
        else if (a == L"--capture" && i + 1 < argc) {
            std::wstring v = argv[++i];
            size_t x = v.find(L'x');
            if (x != std::wstring::npos) {
                o.captureW = uint32_t(_wtoi(v.substr(0, x).c_str()));
                o.captureH = uint32_t(_wtoi(v.substr(x + 1).c_str()));
                o.captureExplicit = true;
            }
        } else if (a == L"--fps" && i + 1 < argc) o.captureFps = _wtof(argv[++i]);
        else if (a == L"--output" && i + 1 < argc) {
            std::wstring v = argv[++i];
            size_t x = v.find(L'x');
            if (x != std::wstring::npos) { o.outW = uint32_t(_wtoi(v.substr(0, x).c_str())); o.outH = uint32_t(_wtoi(v.substr(x + 1).c_str())); }
        } else if (a == L"--dlaa") o.quality = NVSDK_NGX_PerfQuality_Value_DLAA;
        else if (a == L"--performance") o.quality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        else if (a == L"--nr-direct") o.tryDirectNR = true;
    }
    LocalFree(argv);
    return o;
}

// ---------------------------------------------------------------------------
// Camera picker: a small modal used at startup when more than one camera exists.
// ---------------------------------------------------------------------------
namespace picker {

struct State {
    const std::vector<CameraDevice>* devices = nullptr;
    int selection = -1;
    bool done = false;
    HWND list = nullptr;
};

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    if (!st) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_CREATE: {
        st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                   12, 12, 396, 160, h, (HMENU)1, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        228, 184, 88, 28, h, (HMENU)IDOK, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                        322, 184, 88, 28, h, (HMENU)IDCANCEL, nullptr, nullptr);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(h, [](HWND c, LPARAM p) -> BOOL {
            SendMessageW(c, WM_SETFONT, WPARAM(p), TRUE); return TRUE;
        }, LPARAM(f));
        for (const auto& d : *st->devices)
            SendMessageW(st->list, LB_ADDSTRING, 0, LPARAM(d.name.c_str()));
        SendMessageW(st->list, LB_SETCURSEL, 0, 0);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id == IDOK || (id == 1 && HIWORD(w) == LBN_DBLCLK)) {
            st->selection = int(SendMessageW(st->list, LB_GETCURSEL, 0, 0));
            st->done = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) { st->selection = -1; st->done = true; DestroyWindow(h); return 0; }
        return 0;
    }
    case WM_CLOSE: st->selection = -1; st->done = true; DestroyWindow(h); return 0;
    case WM_DESTROY: st->done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int Show(HINSTANCE hi, const std::vector<CameraDevice>& devices) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW c{};
        c.lpfnWndProc = Proc;
        c.hInstance = hi;
        c.lpszClassName = L"DLSSCamPickerClass";
        c.hCursor = LoadCursor(nullptr, IDC_ARROW);
        c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&c);
        registered = true;
    }
    State st;
    st.devices = &devices;
    RECT rc{0, 0, 424, 226};
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DLSSCamPickerClass", L"Select camera",
                             WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hi, &st);
    if (!h) return devices.empty() ? -1 : 0;
    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return st.selection;
}

} // namespace picker

// ---------------------------------------------------------------------------

class CamApp {
public:
    explicit CamApp(AppOptions o) : m_opt(std::move(o)) {}
    ~CamApp() {
        SaveSettings();
        UnregisterOverlayHotkeys();
        Stop();
        if (m_font) DeleteObject(m_font);
    }

    bool Create(HINSTANCE hi) {
        m_hinst = hi;
        LoadSettings();
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);

        WNDCLASSW r{};
        r.style = CS_DBLCLKS | CS_OWNDC;
        r.lpfnWndProc = RenderWndProcStatic;
        r.hInstance = hi;
        r.lpszClassName = L"DLSSCamRenderClass";
        r.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&r);

        WNDCLASSW v{};
        v.lpfnWndProc = ViewportWndProcStatic;
        v.hInstance = hi;
        v.lpszClassName = L"DLSSCamViewportClass";
        v.hCursor = LoadCursor(nullptr, IDC_ARROW);
        v.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&v);

        WNDCLASSW w{};
        w.lpfnWndProc = WndProcStatic;
        w.hInstance = hi;
        w.lpszClassName = L"DLSSCamDemoClass";
        w.hCursor = LoadCursor(nullptr, IDC_ARROW);
        w.hbrBackground = CreateSolidBrush(RGB(18, 19, 21));
        RegisterClassW(&w);

        // An explicit --capture wins over the saved mode.
        if (m_opt.captureExplicit) m_captureIndex = NearestCaptureMode(m_opt.captureW, m_opt.captureH);
        m_captureW = kCaptureModes[m_captureIndex].width;
        m_captureH = kCaptureModes[m_captureIndex].height;

        m_devices = CameraSource::Enumerate();
        LOG("Camera devices found: " << m_devices.size());
        for (size_t i = 0; i < m_devices.size(); ++i)
            LOG("  [" << i << "] " << Narrow(m_devices[i].name));

        RECT rc{0, 0, 1440, 880};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
        m_hwnd = CreateWindowExW(0, w.lpszClassName, L"DLSSCamDemo",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                 nullptr, BuildMenu(), hi, this);
        if (!m_hwnd) return false;

        BOOL dark = TRUE;
        DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark));
        DWORD corner = 2;
        DwmSetWindowAttribute(m_hwnd, 33, &corner, sizeof(corner));
        RegisterOverlayHotkeys();

        m_viewport = CreateWindowExW(0, v.lpszClassName, nullptr,
                                     WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 100,
                                     m_hwnd, nullptr, hi, nullptr);
        m_renderWnd = CreateWindowExW(0, r.lpszClassName, nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 100,
                                      m_viewport, nullptr, hi, this);
        m_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ShowWindow(m_viewport, SW_HIDE);
        Layout();

        if (m_devices.empty()) {
            MessageBoxW(m_hwnd, L"No camera was found.", L"DLSSCamDemo", MB_ICONERROR);
            return true;   // stay open so the log/menu are still reachable
        }

        int index = m_opt.deviceIndex;
        if (index < 0) index = ResolveSavedDevice();
        if (index < 0) index = (m_devices.size() == 1) ? 0 : picker::Show(hi, m_devices);
        if (index < 0 || size_t(index) >= m_devices.size()) index = 0;
        Start(size_t(index));
        return true;
    }

    void Tick() {
        if (!m_camera.IsOpen() || !m_renderer) {
            TryReopenCamera();
            RepresentIdle();
            return;
        }

        VideoFrame frame;
        if (!m_camera.TryGetLatest(frame)) {
            RepresentIdle();
            return;
        }
        m_lastFrameArrival = Clock::now();

        // Pacing is whatever the camera delivers. dt drives the DLSS frame time and
        // detects a stall (device hiccup, USB reset, sleep/resume) that must not be
        // fed to the temporal history as if it were normal motion.
        double dtSeconds = 1.0 / std::max(1.0, m_camera.FrameRate());
        if (m_lastTimestamp >= 0 && frame.timestamp100ns > m_lastTimestamp) {
            const double d = double(frame.timestamp100ns - m_lastTimestamp) * 1e-7;
            if (d > 0.0005 && d < 5.0) dtSeconds = d;
        }
        const bool stalled = (m_lastTimestamp >= 0) && (dtSeconds > 0.5);
        const bool reset = m_forceReset || stalled || m_lastTimestamp < 0;
        if (stalled) LOG("Camera stall detected (" << dtSeconds << " s); resetting temporal history.");

        if (m_mask.segmentation && m_segmenter.Available()) {
            m_segmenter.Submit(frame.bgra.data(), m_camera.Width(), m_camera.Height());
            if (m_segmenter.TryGetMask(m_matte))
                m_renderer->UpdatePersonMask(m_matte.data(), m_segmenter.MaskWidth(), m_segmenter.MaskHeight());
        }

        if (RenderFrame(frame, reset, float(dtSeconds * 1000.0))) {
            m_forceReset = false;
            m_lastTimestamp = frame.timestamp100ns;
            ++m_fpsWindowFrames;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - m_fpsWindowStart).count();
            if (elapsed >= 0.75) {
                m_displayFps = double(m_fpsWindowFrames) / elapsed;
                m_fpsWindowFrames = 0;
                m_fpsWindowStart = now;
            }
            UpdateLatency(frame.captureQpc);
        }

        LogStats();
        if ((++m_uiTick % 15) == 0) { UpdateTitle(); InvalidateControls(); }
    }

    bool Running() const { return m_running; }
    HANDLE FrameEvent() const { return m_camera.FrameEvent(); }

private:
    // ---- settings ---------------------------------------------------------
    std::filesystem::path SettingsPath() const {
        wchar_t p[MAX_PATH]{};
        DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
        if (!n || n >= std::size(p)) return std::filesystem::current_path() / L"DLSSCamDemo.ini";
        return std::filesystem::path(p).parent_path() / L"DLSSCamDemo.ini";
    }
    void LoadSettings() {
        wchar_t buf[512]{};
        GetPrivateProfileStringW(L"camera", L"device", L"", buf, DWORD(std::size(buf)),
                                 SettingsPath().c_str());
        m_savedDeviceName = buf;
        m_mirror = GetPrivateProfileIntW(L"view", L"mirror", 1, SettingsPath().c_str()) != 0;
        m_captureIndex = size_t(std::clamp<int>(
            GetPrivateProfileIntW(L"camera", L"captureMode", 0, SettingsPath().c_str()),
            0, int(std::size(kCaptureModes)) - 1));
        m_compare = D3D12Renderer::CompareMode(std::clamp<int>(
            GetPrivateProfileIntW(L"view", L"compare", 1, SettingsPath().c_str()), 0, 2));
        m_mask.enabled = GetPrivateProfileIntW(L"mask", L"enabled", 0, SettingsPath().c_str()) != 0;
        m_maskSegmentationWanted = GetPrivateProfileIntW(L"mask", L"segmentation", 0, SettingsPath().c_str()) != 0;
        m_mask.radiusX = ReadIniFloat(L"mask", L"radiusX", m_mask.radiusX);
        m_mask.radiusY = ReadIniFloat(L"mask", L"radiusY", m_mask.radiusY);
        m_mask.centerX = ReadIniFloat(L"mask", L"centerX", m_mask.centerX);
        m_mask.centerY = ReadIniFloat(L"mask", L"centerY", m_mask.centerY);
        m_mask.feather = ReadIniFloat(L"mask", L"feather", m_mask.feather);
        m_mask.strength = ReadIniFloat(L"mask", L"strength", m_mask.strength);
        m_neuralWarmth = ReadIniFloat(L"neural", L"warmth", m_neuralWarmth);
        m_neuralExposure = ReadIniFloat(L"neural", L"exposure", m_neuralExposure);
    }
    float ReadIniFloat(const wchar_t* section, const wchar_t* key, float fallback) const {
        wchar_t buf[64]{};
        GetPrivateProfileStringW(section, key, L"", buf, DWORD(std::size(buf)), SettingsPath().c_str());
        if (!buf[0]) return fallback;
        return float(_wtof(buf));
    }
    void WriteIniFloat(const wchar_t* section, const wchar_t* key, float value) const {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.4f", value);
        WritePrivateProfileStringW(section, key, buf, SettingsPath().c_str());
    }
    void SaveSettings() const {
        if (m_savedDeviceName.empty() && m_camera.DeviceName().empty()) return;
        const std::wstring name = m_camera.DeviceName().empty() ? m_savedDeviceName : m_camera.DeviceName();
        WritePrivateProfileStringW(L"camera", L"device", name.c_str(), SettingsPath().c_str());
        WritePrivateProfileStringW(L"camera", L"captureMode",
                                   std::to_wstring(m_captureIndex).c_str(), SettingsPath().c_str());
        WritePrivateProfileStringW(L"view", L"mirror", m_mirror ? L"1" : L"0", SettingsPath().c_str());
        WritePrivateProfileStringW(L"view", L"compare",
                                   std::to_wstring(int(m_compare)).c_str(), SettingsPath().c_str());
        WritePrivateProfileStringW(L"mask", L"enabled", m_mask.enabled ? L"1" : L"0", SettingsPath().c_str());
        WritePrivateProfileStringW(L"mask", L"segmentation", m_mask.segmentation ? L"1" : L"0", SettingsPath().c_str());
        WriteIniFloat(L"mask", L"radiusX", m_mask.radiusX);
        WriteIniFloat(L"mask", L"radiusY", m_mask.radiusY);
        WriteIniFloat(L"mask", L"centerX", m_mask.centerX);
        WriteIniFloat(L"mask", L"centerY", m_mask.centerY);
        WriteIniFloat(L"mask", L"feather", m_mask.feather);
        WriteIniFloat(L"mask", L"strength", m_mask.strength);
        WriteIniFloat(L"neural", L"warmth", m_neuralWarmth);
        WriteIniFloat(L"neural", L"exposure", m_neuralExposure);
    }
    int ResolveSavedDevice() const {
        if (m_savedDeviceName.empty()) return -1;
        for (size_t i = 0; i < m_devices.size(); ++i)
            if (m_devices[i].name == m_savedDeviceName) return int(i);
        return -1;
    }

    // ---- lifecycle --------------------------------------------------------
    bool Start(size_t deviceIndex) {
        Stop();
        if (deviceIndex >= m_devices.size()) return false;
        const CameraDevice& dev = m_devices[deviceIndex];
        LOG("Opening camera: " << Narrow(dev.name));

        if (!m_camera.Open(dev, m_captureW, m_captureH, m_opt.captureFps)) {
            MessageBoxW(m_hwnd, L"Could not open this camera. See DLSSCamDemo.log.",
                        L"DLSSCamDemo", MB_ICONERROR);
            return false;
        }
        m_deviceIndex = deviceIndex;
        m_savedDeviceName = dev.name;

        const uint32_t srcW = m_camera.Width(), srcH = m_camera.Height();
        auto [ow, oh] = OutputForAspect(m_camera.DisplayAspectRatio(), m_opt.outW, m_opt.outH);
        const auto [gridW, gridH] = TemporalGuideGenerator::AnalysisGrid(srcW, srcH, m_camera.FrameRate());

        ShowWindow(m_viewport, SW_SHOW);
        Layout();
        m_renderer = std::make_unique<D3D12Renderer>();
        if (!m_renderer->Initialize(m_renderWnd, srcW, srcH, ow, oh, gridW, gridH, m_opt.quality, m_opt.tryDirectNR)) {
            MessageBoxW(m_hwnd, L"Could not initialize D3D12/NGX. See DLSSCamDemo.log.",
                        L"DLSSCamDemo", MB_ICONERROR);
            m_renderer.reset();
            m_camera.Close();
            ShowWindow(m_viewport, SW_HIDE);
            return false;
        }
        m_renderer->SetMirror(m_mirror);
        m_renderer->SetCompareMode(m_compare);
        m_renderer->SetMask(m_mask);
        ApplyNeuralGain();
        m_guides.Reset();
        m_forceReset = true;
        m_lastTimestamp = -1;
        m_fpsWindowStart = Clock::now();
        m_fpsWindowFrames = 0;
        m_displayFps = 0.0;
        m_latencyMs = 0.0;
        m_resetCount = 0;
        // Honour the saved segmentation preference once the pipeline is up.
        if (m_maskSegmentationWanted && !m_mask.segmentation) {
            m_maskSegmentationWanted = false;
            ToggleSegmentation();
        }
        SyncMenuChecks();
        UpdateTitle();
        Layout();
        InvalidateRect(m_hwnd, nullptr, TRUE);
        return true;
    }

    void Stop() {
        m_camera.Close();
        if (m_renderer) { m_renderer->WaitGPU(); m_renderer.reset(); }
        m_guides.Reset();
        m_lastTimestamp = -1;
        if (m_viewport) ShowWindow(m_viewport, SW_HIDE);
    }

    static double MsSince(int64_t qpc) {
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        return freq.QuadPart ? double(now.QuadPart - qpc) * 1000.0 / double(freq.QuadPart) : 0.0;
    }
    static int64_t Qpc() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }

    bool RenderFrame(const VideoFrame& f, bool reset, float frameTimeMs) {
        GuideFrame g;
        const int64_t tGuide = Qpc();
        if (!m_guides.Generate(f.bgra.data(), m_camera.Width(), m_camera.Height(),
                               m_renderer->DLSSInputW(), m_renderer->DLSSInputH(),
                               m_camera.FrameRate(), reset, g))
            return false;
        const double guideMs = MsSince(tGuide);

        const bool dlssReset = reset || !g.hasHistory;
        if (dlssReset) ++m_resetCount;
        m_exposureDelta = g.exposureDelta;
        const int64_t tRender = Qpc();
        const bool ok = m_renderer->RenderFrame(f.bgra.data(), f.bgra.size(),
                                                g.guideGridRGBA32F.data(),
                                                g.guideGridRGBA32F.size() * sizeof(float),
                                                g.gridW, g.gridH, dlssReset, frameTimeMs);
        const double renderMs = MsSince(tRender);

        m_guideMs = m_guideMs <= 0.0 ? guideMs : m_guideMs * 0.9 + guideMs * 0.1;
        m_renderMs = m_renderMs <= 0.0 ? renderMs : m_renderMs * 0.9 + renderMs * 0.1;
        m_lastGlobalX = g.globalMotionX;
        m_lastGlobalY = g.globalMotionY;
        // Global motion is zero for a fixed camera by definition; what matters for the
        // neural renderer is whether any local block actually moved.
        float peak = 0.0f;
        for (size_t i = 0; i + 1 < g.guideGridRGBA32F.size(); i += 4)
            peak = std::max(peak, std::abs(g.guideGridRGBA32F[i]) + std::abs(g.guideGridRGBA32F[i + 1]));
        m_peakLocalMV = peak;
        return ok;
    }

    // Changing the capture size changes the DLSS contract, so the whole pipeline is
    // rebuilt. The camera picks the nearest mode it actually offers, which the status
    // strip reports -- a request for 320x180 can legitimately land on 640x360.
    void SetCaptureMode(size_t index) {
        if (index >= std::size(kCaptureModes) || index == m_captureIndex) return;
        m_captureIndex = index;
        m_captureW = kCaptureModes[index].width;
        m_captureH = kCaptureModes[index].height;
        LOG("Capture mode -> " << m_captureW << "x" << m_captureH);
        if (m_deviceIndex != SIZE_MAX) Start(m_deviceIndex);
        SyncMenuChecks();
        InvalidateControls();
    }

    void CycleCaptureMode() {
        SetCaptureMode((m_captureIndex + 1) % std::size(kCaptureModes));
    }

    // Nearest listed mode by pixel count, so a --capture value lands on a menu entry.
    static size_t NearestCaptureMode(uint32_t w, uint32_t h) {
        size_t best = 0;
        double bestErr = 1e30;
        for (size_t i = 0; i < std::size(kCaptureModes); ++i) {
            const double err = std::abs(std::log((double(kCaptureModes[i].width) * kCaptureModes[i].height)
                                                 / std::max(1.0, double(w) * h)));
            if (err < bestErr) { bestErr = err; best = i; }
        }
        return best;
    }

    // A live source can vanish mid-session (another app grabs it, USB blip, resume
    // from sleep). Keep trying, at a calm interval, so the demo recovers on its own.
    void TryReopenCamera() {
        if (m_deviceIndex == SIZE_MAX || m_camera.IsOpen()) return;
        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - m_lastReopenAttempt).count() < 3.0) return;
        m_lastReopenAttempt = now;

        // Re-enumerate first: a device that came back after a USB reset can carry a
        // different symbolic link. Do not tear the renderer down before the camera is
        // actually back, or a permanently missing device would rebuild the swapchain
        // every few seconds and take the ReShade overlay with it.
        auto devices = CameraSource::Enumerate();
        size_t index = SIZE_MAX;
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].name == m_savedDeviceName) { index = i; break; }
        if (index == SIZE_MAX) return;

        const uint32_t prevW = m_camera.Width(), prevH = m_camera.Height();
        if (!m_camera.Open(devices[index], m_captureW, m_captureH, m_opt.captureFps)) return;
        m_devices = std::move(devices);
        m_deviceIndex = index;
        LOG("Camera: reopened " << Narrow(m_savedDeviceName));

        if (!m_renderer || m_camera.Width() != prevW || m_camera.Height() != prevH) {
            Start(m_deviceIndex);   // capture size changed, so the DLSS contract must be rebuilt
        } else {
            m_guides.Reset();
            m_forceReset = true;
            m_lastTimestamp = -1;
        }
    }

    // Keeps the swapchain (and therefore the ReShade overlay) alive when no new
    // camera frame has arrived, without advancing DLSS temporal history.
    void RepresentIdle() {
        if (!m_renderer) return;
        const auto now = Clock::now();
        // Only after the camera has actually gone quiet. Presenting on every empty
        // poll steals present slots from the render path and halves the frame rate.
        if (std::chrono::duration<double>(now - m_lastFrameArrival).count() < 0.1) return;
        if (std::chrono::duration<double>(now - m_lastIdlePresent).count() < 1.0 / 30.0) return;
        m_renderer->PresentCurrent();
        m_lastIdlePresent = now;
    }

    // Periodic health line. The demo is judged on sustained frame rate and end-to-end
    // latency, and both are invisible from the NGX log alone.
    void LogStats() {
        const auto now = Clock::now();
        if (std::chrono::duration<double>(now - m_lastStatsLog).count() < 5.0) return;
        m_lastStatsLog = now;
        LOG("Stats: " << std::fixed << std::setprecision(1) << m_displayFps << " fps"
            << " | latency " << int(std::lround(m_latencyMs)) << " ms"
            << " | captured " << m_camera.CapturedFrames()
            << " | capture drop " << m_camera.DroppedFrames()
            << " | guide " << std::setprecision(1) << m_guideMs << " ms"
            << " | render " << m_renderMs << " ms"
            << " | NGX eval " << (m_renderer ? m_renderer->DLSSEvaluations() : 0)
            << " | MV global " << int(std::lround(m_lastGlobalX)) << "," << int(std::lround(m_lastGlobalY))
            << " peak " << std::setprecision(2) << m_peakLocalMV << "px"
            << " | matte " << (m_mask.segmentation
                                  ? (std::to_string(int(std::lround(m_segmenter.LastInferenceMs()))) + " ms/"
                                     + std::to_string(m_segmenter.Inferences()))
                                  : std::string("off"))
            << " | resets " << m_resetCount
            << " | exposure " << std::setprecision(3) << m_exposureDelta);
    }

    void UpdateLatency(int64_t captureQpc) {
        if (!captureQpc) return;
        LARGE_INTEGER freq{}, now{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) return;
        const double ms = double(now.QuadPart - captureQpc) * 1000.0 / double(freq.QuadPart);
        // Exponential average: the raw per-frame number jitters too much to read.
        m_latencyMs = (m_latencyMs <= 0.0) ? ms : m_latencyMs * 0.9 + ms * 0.1;
    }

    static std::wstring PlainValue(float v) {
        wchar_t b[32]{};
        swprintf_s(b, L"%.2f", v);
        return b;
    }

    static std::pair<uint32_t, uint32_t> OutputForAspect(double dar, uint32_t maxW, uint32_t maxH) {
        const double box = double(maxW) / double(maxH);
        uint32_t w, h;
        if (dar >= box) { w = maxW; h = uint32_t(std::lround(double(w) / dar)); }
        else { h = maxH; w = uint32_t(std::lround(double(h) * dar)); }
        w = std::max(64u, w & ~1u);
        h = std::max(64u, h & ~1u);
        return {w, h};
    }

    // ---- window plumbing --------------------------------------------------
    HMENU BuildMenu() {
        HMENU bar = CreateMenu();
        m_cameraMenu = CreatePopupMenu();
        for (size_t i = 0; i < m_devices.size(); ++i)
            AppendMenuW(m_cameraMenu, MF_STRING, IDM_CAMERA_BASE + i, m_devices[i].name.c_str());
        if (m_devices.empty()) AppendMenuW(m_cameraMenu, MF_STRING | MF_GRAYED, 0, L"(no camera found)");
        AppendMenuW(m_cameraMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_cameraMenu, MF_STRING, IDM_EXIT, L"E&xit");
        AppendMenuW(bar, MF_POPUP, UINT_PTR(m_cameraMenu), L"&Camera");

        m_captureMenu = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kCaptureModes); ++i)
            AppendMenuW(m_captureMenu, MF_STRING, IDM_CAPTURE_BASE + i, kCaptureModes[i].label);
        AppendMenuW(bar, MF_POPUP, UINT_PTR(m_captureMenu), L"Ca&pture");

        m_viewMenu = CreatePopupMenu();
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_FINAL, L"&Final\t1");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_INPUT, L"DLSS &input\t2");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_MV, L"&Motion vectors\t3");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_DEPTH, L"&Depth\t4");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_MASK, L"Bias m&ask\t5");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_MASKPREVIEW, L"&Person mask\t6");
        AppendMenuW(m_viewMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_SPLIT, L"A/B &wipe\tS");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_SIDEBYSIDE, L"A/B side-by-si&de\tS");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_VIEW_MIRROR, L"&Mirror\tM");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_MASK_TOGGLE, L"Oval mask &on	K");
        AppendMenuW(m_viewMenu, MF_STRING, IDM_MASK_SEGMENT, L"Person se&gmentation	G");
        AppendMenuW(bar, MF_POPUP, UINT_PTR(m_viewMenu), L"&View");

        m_dlssMenu = CreatePopupMenu();
        AppendMenuW(m_dlssMenu, MF_STRING, IDM_DLSS_TOGGLE, L"DLSS &enabled\tD");
        AppendMenuW(m_dlssMenu, MF_STRING, IDM_DLSS_RECREATE, L"&Recreate NGX feature\tF6");
        AppendMenuW(m_dlssMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_dlssMenu, MF_STRING, IDM_NEUTRAL_RESET, L"Reset neural &white balance\t0");
        AppendMenuW(bar, MF_POPUP, UINT_PTR(m_dlssMenu), L"&DLSS");
        return bar;
    }

    void SyncMenuChecks() {
        if (m_viewMenu) {
            const int views[] = {IDM_VIEW_FINAL, IDM_VIEW_INPUT, IDM_VIEW_MV, IDM_VIEW_DEPTH,
                                 IDM_VIEW_MASK, IDM_VIEW_MASKPREVIEW};
            const auto current = m_renderer ? m_renderer->GetDebugView() : D3D12Renderer::DebugView::Final;
            for (int i = 0; i < 6; ++i)
                CheckMenuItem(m_viewMenu, views[i], MF_BYCOMMAND | (int(current) == i ? MF_CHECKED : MF_UNCHECKED));
        }
        if (m_viewMenu) {
            const auto cmode = m_renderer ? m_renderer->GetCompareMode() : D3D12Renderer::CompareMode::Off;
            CheckMenuItem(m_viewMenu, IDM_VIEW_SPLIT,
                          MF_BYCOMMAND | (cmode == D3D12Renderer::CompareMode::Wipe ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m_viewMenu, IDM_VIEW_SIDEBYSIDE,
                          MF_BYCOMMAND | (cmode == D3D12Renderer::CompareMode::SideBySide ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m_viewMenu, IDM_VIEW_MIRROR, MF_BYCOMMAND | (m_mirror ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m_viewMenu, IDM_MASK_TOGGLE, MF_BYCOMMAND | (m_mask.enabled ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(m_viewMenu, IDM_MASK_SEGMENT,
                          MF_BYCOMMAND | (m_mask.segmentation ? MF_CHECKED : MF_UNCHECKED));
        }
        if (m_dlssMenu)
            CheckMenuItem(m_dlssMenu, IDM_DLSS_TOGGLE,
                          MF_BYCOMMAND | ((m_renderer && m_renderer->DLSSEnabled()) ? MF_CHECKED : MF_UNCHECKED));
        if (m_captureMenu)
            for (size_t i = 0; i < std::size(kCaptureModes); ++i)
                CheckMenuItem(m_captureMenu, UINT(IDM_CAPTURE_BASE + i),
                              MF_BYCOMMAND | (i == m_captureIndex ? MF_CHECKED : MF_UNCHECKED));
        if (m_cameraMenu)
            for (size_t i = 0; i < m_devices.size(); ++i)
                CheckMenuItem(m_cameraMenu, UINT(IDM_CAMERA_BASE + i),
                              MF_BYCOMMAND | (i == m_deviceIndex ? MF_CHECKED : MF_UNCHECKED));
    }

    void RegisterOverlayHotkeys() {
        auto reg = [&](int id, UINT mods, UINT vk, const char* name) {
            if (!RegisterHotKey(m_hwnd, id, mods | MOD_NOREPEAT, vk))
                LOG("Overlay hotkey unavailable: " << name << " winerr=" << GetLastError());
        };
        reg(HK_DLSS, MOD_CONTROL | MOD_ALT, 'D', "Ctrl+Alt+D");
        reg(HK_RECREATE, MOD_CONTROL | MOD_ALT, VK_F6, "Ctrl+Alt+F6");
        reg(HK_MIRROR, MOD_CONTROL | MOD_ALT, VK_F7, "Ctrl+Alt+F7");
        reg(HK_CAPTURE, MOD_CONTROL | MOD_ALT, VK_F9, "Ctrl+Alt+F9");
        reg(HK_MASK, MOD_CONTROL | MOD_ALT, 'K', "Ctrl+Alt+K");
        reg(HK_SEGMENT, MOD_CONTROL | MOD_ALT, 'G', "Ctrl+Alt+G");
        reg(HK_CAPTURE_MODE, MOD_CONTROL | MOD_ALT, 'R', "Ctrl+Alt+R");
        reg(HK_COOLER, MOD_CONTROL | MOD_ALT, VK_OEM_4, "Ctrl+Alt+[");
        reg(HK_WARMER, MOD_CONTROL | MOD_ALT, VK_OEM_6, "Ctrl+Alt+]");
    }
    void UnregisterOverlayHotkeys() {
        if (!m_hwnd) return;
        for (int id : {HK_DLSS, HK_RECREATE, HK_MIRROR, HK_CAPTURE, HK_MASK, HK_SEGMENT, HK_CAPTURE_MODE,
                       HK_WARMER, HK_COOLER})
            UnregisterHotKey(m_hwnd, id);
    }

    void SetDebugView(D3D12Renderer::DebugView v) {
        if (!m_renderer) return;
        m_renderer->SetDebugView(v);
        SyncMenuChecks();
        InvalidateControls();
    }

    void ApplyMask() {
        if (m_renderer) m_renderer->SetMask(m_mask);
        InvalidateControls();
    }

    // The matte model is loaded lazily: a demo that never turns segmentation on
    // should not pay a 14 MB model load or a busy inference thread.
    // The neural passes leave a consistent cool cast. Warmth is expressed the usual
    // way -- lift red, drop blue -- and `exposure` compensates the brightness lift the
    // same passes introduce. Both are applied after DLSS, to the neural image only.
    void ApplyNeuralGain() {
        D3D12Renderer::NeuralGain g;
        const float t = m_neuralWarmth;
        g.r = m_neuralExposure * (1.0f + 0.12f * t);
        g.g = m_neuralExposure;
        g.b = m_neuralExposure * (1.0f - 0.12f * t);
        if (m_renderer) m_renderer->SetNeuralGain(g);
        InvalidateControls();
    }

    void AdjustNeuralWarmth(int direction) {
        m_neuralWarmth = std::clamp(m_neuralWarmth + direction * 0.05f, -2.0f, 2.0f);
        LOG("Neural white balance: warmth=" << m_neuralWarmth << " exposure=" << m_neuralExposure);
        ApplyNeuralGain();
    }

    void ResetNeuralGain() {
        m_neuralWarmth = 0.0f;
        m_neuralExposure = 1.0f;
        LOG("Neural white balance reset to neutral.");
        ApplyNeuralGain();
    }

    void ToggleSegmentation() {
        if (!m_mask.segmentation) {
            if (!m_segmenter.Available() && !StartSegmenter()) return;
            m_mask.segmentation = true;
        } else {
            m_mask.segmentation = false;
        }
        ApplyMask();
        SyncMenuChecks();
    }

    bool StartSegmenter() {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path model =
            std::filesystem::path(exe).parent_path() / L"rvm_mobilenetv3_fp32.onnx";
        // 512x288 at ratio 0.5 measured ~8 ms per frame on this machine: the network
        // runs at half that size and the matte is refined back up.
        return m_segmenter.Initialize(model.wstring(), 512, 288, 0.5f, 4);
    }

    void ToggleMask() {
        m_mask.enabled = !m_mask.enabled;
        ApplyMask();
        SyncMenuChecks();
    }

    // Wheel resizes the oval, Shift+wheel changes how strongly the reconstruction
    // replaces the original, Ctrl+wheel softens the edge.
    void AdjustMask(int wheelDelta, bool shift, bool ctrl) {
        const float step = (wheelDelta > 0) ? 1.0f : -1.0f;
        if (shift) m_mask.strength = std::clamp(m_mask.strength + step * 0.05f, 0.0f, 1.0f);
        else if (ctrl) m_mask.feather = std::clamp(m_mask.feather + step * 0.05f, 0.02f, 0.95f);
        else {
            const float scale = (step > 0.0f) ? 1.06f : 1.0f / 1.06f;
            m_mask.radiusX = std::clamp(m_mask.radiusX * scale, 0.03f, 1.5f);
            m_mask.radiusY = std::clamp(m_mask.radiusY * scale, 0.03f, 1.5f);
        }
        ApplyMask();
    }

    void SetMaskCenterFromClient(int x, int y) {
        if (!m_renderWnd) return;
        RECT rc{};
        GetClientRect(m_renderWnd, &rc);
        m_mask.centerX = std::clamp(float(x) / float(std::max<LONG>(1, rc.right - rc.left)), 0.0f, 1.0f);
        m_mask.centerY = std::clamp(float(y) / float(std::max<LONG>(1, rc.bottom - rc.top)), 0.0f, 1.0f);
        ApplyMask();
    }

    void SetCompareMode(D3D12Renderer::CompareMode mode) {
        m_compare = mode;
        if (m_renderer) m_renderer->SetCompareMode(mode);
        SyncMenuChecks();
        InvalidateControls();
    }

    // S cycles wipe -> side-by-side -> off, so one key reaches every comparison.
    void CycleCompareMode() {
        using M = D3D12Renderer::CompareMode;
        SetCompareMode(m_compare == M::Wipe ? M::SideBySide : (m_compare == M::SideBySide ? M::Off : M::Wipe));
    }

    void ToggleMirror() {
        m_mirror = !m_mirror;
        if (m_renderer) m_renderer->SetMirror(m_mirror);
        SyncMenuChecks();
        InvalidateControls();
    }

    // Screen x on the render window -> wipe position. The renderer clamps it away
    // from the edges so one side never disappears entirely.
    void SetSplitFromClientX(int x) {
        if (!m_renderer || !m_renderWnd) return;
        RECT rc{};
        GetClientRect(m_renderWnd, &rc);
        const int w = std::max<LONG>(1, rc.right - rc.left);
        m_renderer->SetSplitPosition(float(x) / float(w));
        if (!m_camera.IsOpen()) m_renderer->PresentCurrent();
    }

    // Writes the still A/B pair next to the executable, into captures/.
    void SaveComparison() {
        if (!m_renderer) return;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path dir = std::filesystem::path(exe).parent_path() / L"captures";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::wstring message;
        m_captureMessage = m_renderer->SaveComparisonPNG(dir.wstring(), message) ? message
                                                                                 : (L"capture failed: " + message);
        m_captureMessageAt = Clock::now();
        InvalidateControls();
    }

    void ToggleDLSS() {
        if (!m_renderer) return;
        m_renderer->SetDLSS(!m_renderer->DLSSEnabled());
        SyncMenuChecks();
        InvalidateControls();
    }

    void Layout() {
        if (!m_hwnd) return;
        RECT c{};
        GetClientRect(m_hwnd, &c);
        const int stripH = m_fullscreen ? 0 : CONTROL_H;
        const int vh = std::max<int>(0, c.bottom - stripH);
        if (m_viewport) MoveWindow(m_viewport, 0, 0, c.right, vh, TRUE);
        if (!m_renderWnd || !m_renderer) return;

        // Fit the DLSS output rectangle into the viewport, preserving aspect.
        const double ar = double(m_renderer->OutputW()) / std::max(1u, m_renderer->OutputH());
        int w = c.right, h = int(std::lround(w / ar));
        if (h > vh) { h = vh; w = int(std::lround(h * ar)); }
        MoveWindow(m_renderWnd, (c.right - w) / 2, (vh - h) / 2, std::max(1, w), std::max(1, h), TRUE);
    }

    void ToggleFullscreen() {
        m_fullscreen = !m_fullscreen;
        if (m_fullscreen) {
            GetWindowRect(m_hwnd, &m_restoreRect);
            m_restoreStyle = GetWindowLongW(m_hwnd, GWL_STYLE);
            SetMenu(m_hwnd, nullptr);
            SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
        } else {
            SetWindowLongW(m_hwnd, GWL_STYLE, m_restoreStyle);
            SetMenu(m_hwnd, BuildMenu());
            SyncMenuChecks();
            SetWindowPos(m_hwnd, nullptr, m_restoreRect.left, m_restoreRect.top,
                         m_restoreRect.right - m_restoreRect.left,
                         m_restoreRect.bottom - m_restoreRect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
        }
        Layout();
    }

    void InvalidateControls() {
        if (!m_hwnd || m_fullscreen) return;
        RECT c{};
        GetClientRect(m_hwnd, &c);
        RECT strip{0, std::max<LONG>(0, c.bottom - CONTROL_H), c.right, c.bottom};
        InvalidateRect(m_hwnd, &strip, FALSE);
    }

    void UpdateTitle() {
        std::wstringstream s;
        s << L"DLSSCamDemo";
        if (m_camera.IsOpen()) {
            s << L" | " << m_camera.DeviceName()
              << L" " << m_camera.Width() << L"x" << m_camera.Height()
              << L" " << m_camera.FormatName();
        }
        if (m_renderer) {
            s << L" | DLSS " << m_renderer->DLSSInputW() << L"x" << m_renderer->DLSSInputH()
              << L" -> " << m_renderer->OutputW() << L"x" << m_renderer->OutputH()
              << L" | NGX " << (m_renderer->DLSSFeatureCreated() ? L"CREATE OK"
                                : (m_renderer->DLSSAvailable() ? L"READY" : L"FALLBACK"))
              << L" | eval " << m_renderer->DLSSEvaluations();
        }
        SetWindowTextW(m_hwnd, s.str().c_str());
    }

    void PaintControls(HDC dc, const RECT& c) {
        RECT strip{0, std::max<LONG>(0, c.bottom - CONTROL_H), c.right, c.bottom};
        HBRUSH bg = CreateSolidBrush(RGB(24, 25, 28));
        FillRect(dc, &strip, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(224, 226, 230));
        HGDIOBJ old = SelectObject(dc, m_font);

        std::wstringstream l1, l2;
        if (m_camera.IsOpen()) {
            l1 << m_camera.DeviceName() << L"  |  requested "
               << m_captureW << L"x" << m_captureH << L" -> got "
               << m_camera.Width() << L"x" << m_camera.Height()
               << L" " << m_camera.FormatName() << L" @" << int(std::lround(m_camera.FrameRate()))
               << L"  |  capture drop " << m_camera.DroppedFrames();
        } else {
            l1 << L"No camera running. Use the Camera menu.";
        }
        if (m_renderer) {
            l2 << L"DLSS " << m_renderer->DLSSInputW() << L"x" << m_renderer->DLSSInputH()
               << L" -> " << m_renderer->OutputW() << L"x" << m_renderer->OutputH()
               << L"  |  " << (m_renderer->DLSSEnabled() ? L"ON" : L"OFF")
               << L"  |  NGX create " << (m_renderer->DLSSFeatureCreated() ? L"OK" : L"-")
               << L"  eval " << m_renderer->DLSSEvaluations()
               << L"  0x" << std::hex << uint32_t(m_renderer->DLSSLastResult()) << std::dec
               << L"  |  " << std::fixed << std::setprecision(1) << m_displayFps << L" fps"
               << L"  |  latency " << int(std::lround(m_latencyMs)) << L" ms"
               << L"  |  NR " << (m_renderer->NRFeatureCreated() ? L"direct" : m_renderer->NRStatus())
               << L"  |  MV " << int(std::lround(m_lastGlobalX)) << L"," << int(std::lround(m_lastGlobalY))
               << L"  |  " << (m_compare == D3D12Renderer::CompareMode::Wipe ? L"A/B wipe (drag to move)"
                                : m_compare == D3D12Renderer::CompareMode::SideBySide ? L"A/B side-by-side"
                                : L"full")
               << (m_mirror ? L" | mirror" : L"")
               << L"  |  WB " << PlainValue(m_neuralWarmth) << L"/" << PlainValue(m_neuralExposure)
               << (m_mask.enabled
                       ? (L"  |  oval r" + PlainValue(m_mask.radiusX) + L" f" + PlainValue(m_mask.feather)
                          + L" s" + PlainValue(m_mask.strength))
                       : std::wstring(L"  |  oval off"))
               << (m_mask.segmentation
                       ? (L"  |  matte " + std::to_wstring(int(std::lround(m_segmenter.LastInferenceMs()))) + L" ms")
                       : std::wstring(L"  |  matte off"));
        }
        if (!m_captureMessage.empty()) {
            if (std::chrono::duration<double>(Clock::now() - m_captureMessageAt).count() < 5.0)
                l1 << L"   [" << m_captureMessage << L"]";
            else
                m_captureMessage.clear();
        }
        RECT r1{14, strip.top + 8, c.right - 14, strip.top + 28};
        RECT r2{14, strip.top + 30, c.right - 14, strip.top + 52};
        DrawTextW(dc, l1.str().c_str(), -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, RGB(150, 200, 255));
        DrawTextW(dc, l2.str().c_str(), -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }

    LRESULT WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_SIZE: Layout(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT c{};
            GetClientRect(h, &c);
            if (!m_fullscreen) PaintControls(dc, c);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_HOTKEY:
            if (w == HK_DLSS) ToggleDLSS();
            else if (w == HK_RECREATE && m_renderer) m_renderer->RequestDLSSRecreate();
            else if (w == HK_MIRROR) ToggleMirror();
            else if (w == HK_CAPTURE) SaveComparison();
            else if (w == HK_MASK) ToggleMask();
            else if (w == HK_SEGMENT) ToggleSegmentation();
            else if (w == HK_CAPTURE_MODE) CycleCaptureMode();
            else if (w == HK_WARMER) AdjustNeuralWarmth(+1);
            else if (w == HK_COOLER) AdjustNeuralWarmth(-1);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(w);
            if (id >= IDM_CAMERA_BASE && id < IDM_CAMERA_BASE + int(m_devices.size())) {
                Start(size_t(id - IDM_CAMERA_BASE));
                return 0;
            }
            if (id >= IDM_CAPTURE_BASE && id < IDM_CAPTURE_BASE + int(std::size(kCaptureModes))) {
                SetCaptureMode(size_t(id - IDM_CAPTURE_BASE));
                return 0;
            }
            switch (id) {
            case IDM_VIEW_FINAL: SetDebugView(D3D12Renderer::DebugView::Final); return 0;
            case IDM_VIEW_INPUT: SetDebugView(D3D12Renderer::DebugView::Input); return 0;
            case IDM_VIEW_MV: SetDebugView(D3D12Renderer::DebugView::MotionVectors); return 0;
            case IDM_VIEW_DEPTH: SetDebugView(D3D12Renderer::DebugView::Depth); return 0;
            case IDM_VIEW_MASK: SetDebugView(D3D12Renderer::DebugView::BiasMask); return 0;
            case IDM_VIEW_MASKPREVIEW: SetDebugView(D3D12Renderer::DebugView::PersonMask); return 0;
            case IDM_MASK_TOGGLE: ToggleMask(); return 0;
            case IDM_MASK_SEGMENT: ToggleSegmentation(); return 0;
            case IDM_VIEW_SPLIT: SetCompareMode(D3D12Renderer::CompareMode::Wipe); return 0;
            case IDM_VIEW_SIDEBYSIDE: SetCompareMode(D3D12Renderer::CompareMode::SideBySide); return 0;
            case IDM_VIEW_MIRROR: ToggleMirror(); return 0;
            case IDM_DLSS_TOGGLE: ToggleDLSS(); return 0;
            case IDM_DLSS_RECREATE: if (m_renderer) m_renderer->RequestDLSSRecreate(); return 0;
            case IDM_NEUTRAL_RESET: ResetNeuralGain(); return 0;
            case IDM_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); return 0;
            }
            return 0;
        }
        case WM_KEYDOWN:
            switch (w) {
            case '1': SetDebugView(D3D12Renderer::DebugView::Final); return 0;
            case '2': SetDebugView(D3D12Renderer::DebugView::Input); return 0;
            case '3': SetDebugView(D3D12Renderer::DebugView::MotionVectors); return 0;
            case '4': SetDebugView(D3D12Renderer::DebugView::Depth); return 0;
            case '5': SetDebugView(D3D12Renderer::DebugView::BiasMask); return 0;
            case '6': SetDebugView(D3D12Renderer::DebugView::PersonMask); return 0;
            case 'K': ToggleMask(); return 0;
            case 'G': ToggleSegmentation(); return 0;
            case 'R': CycleCaptureMode(); return 0;
            case VK_OEM_4: AdjustNeuralWarmth(-1); return 0;   // '[' cooler
            case VK_OEM_6: AdjustNeuralWarmth(+1); return 0;   // ']' warmer
            case '0': ResetNeuralGain(); return 0;
            case 'D': ToggleDLSS(); return 0;
            case 'S': CycleCompareMode(); return 0;
            case 'M': ToggleMirror(); return 0;
            case VK_F6: if (m_renderer) m_renderer->RequestDLSSRecreate(); return 0;
            case VK_F9: SaveComparison(); return 0;
            case VK_F11: ToggleFullscreen(); return 0;
            case VK_ESCAPE: if (m_fullscreen) ToggleFullscreen(); return 0;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: Stop(); m_running = false; PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
        CamApp* a = nullptr;
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            a = static_cast<CamApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
        } else {
            a = reinterpret_cast<CamApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        return a ? a->WndProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK ViewportWndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            RECT r{};
            GetClientRect(h, &r);
            FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(h, &ps);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    // The render window owns the swapchain ReShade/RenoDX hook. It forwards input to
    // the main window so the overlay and the app agree on who handles a key.
    static LRESULT CALLBACK RenderWndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
        CamApp* a = nullptr;
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            a = static_cast<CamApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
        } else {
            a = reinterpret_cast<CamApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (a) {
            if (m == WM_ERASEBKGND) return 1;
            if (m == WM_PAINT) { PAINTSTRUCT ps{}; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
            if (m == WM_LBUTTONDOWN) {
                SetFocus(a->m_hwnd);
                if (a->m_renderer && a->m_renderer->GetCompareMode() == D3D12Renderer::CompareMode::Wipe) {
                    a->m_draggingSplit = true;
                    SetCapture(h);
                    a->SetSplitFromClientX(GET_X_LPARAM(l));
                }
                return 0;
            }
            if (m == WM_MOUSEMOVE && a->m_draggingSplit) { a->SetSplitFromClientX(GET_X_LPARAM(l)); return 0; }
            if (m == WM_LBUTTONUP && a->m_draggingSplit) { a->m_draggingSplit = false; ReleaseCapture(); return 0; }
            if (m == WM_RBUTTONDOWN) {
                a->m_draggingMask = true;
                SetCapture(h);
                a->SetMaskCenterFromClient(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                return 0;
            }
            if (m == WM_MOUSEMOVE && a->m_draggingMask) { a->SetMaskCenterFromClient(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0; }
            if (m == WM_RBUTTONUP && a->m_draggingMask) { a->m_draggingMask = false; ReleaseCapture(); return 0; }
            if (m == WM_MOUSEWHEEL) {
                a->AdjustMask(GET_WHEEL_DELTA_WPARAM(w),
                              (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                              (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                return 0;
            }
            if (m == WM_LBUTTONDBLCLK) { a->ToggleFullscreen(); return 0; }
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) return SendMessageW(a->m_hwnd, m, w, l);
        }
        return DefWindowProcW(h, m, w, l);
    }

    AppOptions m_opt;
    HINSTANCE m_hinst = nullptr;
    HWND m_hwnd = nullptr, m_viewport = nullptr, m_renderWnd = nullptr;
    HMENU m_cameraMenu = nullptr, m_captureMenu = nullptr, m_viewMenu = nullptr, m_dlssMenu = nullptr;
    HFONT m_font = nullptr;
    bool m_running = true;
    bool m_fullscreen = false;
    bool m_mirror = true;
    D3D12Renderer::CompareMode m_compare = D3D12Renderer::CompareMode::Wipe;
    bool m_draggingSplit = false;
    bool m_draggingMask = false;
    D3D12Renderer::MaskSettings m_mask{};
    bool m_maskSegmentationWanted = false;
    float m_neuralWarmth = 0.0f;      // -2..+2, 0 = untouched
    float m_neuralExposure = 1.0f;    // linear gain on the neural image
    RECT m_restoreRect{};
    LONG m_restoreStyle = 0;

    uint32_t m_captureW = 1280, m_captureH = 720;
    size_t m_captureIndex = 0;
    std::vector<CameraDevice> m_devices;
    size_t m_deviceIndex = SIZE_MAX;
    std::wstring m_savedDeviceName;
    CameraSource m_camera;
    TemporalGuideGenerator m_guides;
    PersonSegmenter m_segmenter;
    std::vector<uint8_t> m_matte;
    std::unique_ptr<D3D12Renderer> m_renderer;

    bool m_forceReset = true;
    int64_t m_lastTimestamp = -1;
    Clock::time_point m_fpsWindowStart = Clock::now();
    Clock::time_point m_lastIdlePresent = Clock::now();
    Clock::time_point m_lastStatsLog = Clock::now();
    Clock::time_point m_lastFrameArrival = Clock::now();
    Clock::time_point m_lastReopenAttempt = Clock::now();
    uint64_t m_fpsWindowFrames = 0;
    uint64_t m_uiTick = 0;
    double m_displayFps = 0.0;
    double m_latencyMs = 0.0;
    double m_guideMs = 0.0;
    uint64_t m_resetCount = 0;
    float m_exposureDelta = 0.0f;
    float m_peakLocalMV = 0.0f;
    std::wstring m_captureMessage;
    Clock::time_point m_captureMessageAt{};
    double m_renderMs = 0.0;
    float m_lastGlobalX = 0.0f, m_lastGlobalY = 0.0f;
};

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { CoUninitialize(); return 1; }

    {
        CamApp app(ParseArgs());
        if (!app.Create(hi)) { MFShutdown(); CoUninitialize(); return 1; }
        MSG msg{};
        bool quit = false;
        while (app.Running() && !quit) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit) break;
            app.Tick();

            // Sleep until the camera publishes a frame or a window message arrives.
            // Polling with Sleep(1) costs a full 15.6 ms timer tick per iteration on
            // a default Windows timer resolution, which alone caps the demo well
            // below the capture rate.
            HANDLE ev = app.FrameEvent();
            const DWORD timeout = 30;
            if (ev) MsgWaitForMultipleObjects(1, &ev, FALSE, timeout, QS_ALLINPUT);
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, timeout, QS_ALLINPUT);
        }
    }

    MFShutdown();
    CoUninitialize();
    return 0;
}
