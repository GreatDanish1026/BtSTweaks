// BtSTweaks: a ReShade add-on that adds HDR to Life is Strange: Before the Storm Remastered
// (Unity 2019.4, IL2CPP, DirectX 11) while keeping the game's colour grade.
//
// How the game draws (draw logs, 2026-10-28): the scene renders into a 16-bit float target,
// Unity's Post-Processing Stack v1 tonemaps it into an 8-bit sRGB texture, the UI is drawn into
// its own multisampled texture that is resolved and composited onto that picture, and one quad
// blits the result to the 8-bit back buffer through an sRGB view.
//
// HDR here:
//  - The swap chain is created as 16-bit float (R16G16B16A16_FLOAT) instead of 8-bit, when HDR is
//    on at launch and Windows HDR is on for the game's display, and Unity's views on the back
//    buffer (sRGB render target, plain render target, shader resource) are made float too. The
//    final blit then writes linear light without banding.
//  - At each present, before ReShade's effects and overlay, src/hdr.hlsl turns the frame into
//    scRGB: re-encode, gamma 2.2 decode, paper white, highlights stretched above the "Highlights
//    start at" knee towards the peak. The swap chain is set to scRGB (DXGI G10 P709) and ReShade
//    is told (set_color_space).
//  - The UI layer's resolved texture (the last full-screen resolve of the frame) is read in the
//    pass; its alpha keeps the UI at paper white.
//  - On DirectX 11 the pass runs in its own device context state (SwapDeviceContextState), so
//    Unity's cached bindings are never disturbed.
//
// The add-on never changes the game's code or files. It writes BtSTweaks.log (and -draws.txt)
// next to the game's exe. Settings are in ReShade.ini [BtSTweaks].

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "hdr_ps.h"  // built from src/hdr.hlsl by build.ps1
#include "hdr_vs.h"
#include "version.h"
#include "il2cpp_probe.hpp"
#include "il2cpp_main.hpp"
#include "effects.hpp"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "HDR for " DT_GAME " that keeps the game's colour grade, with UI kept at paper white.";

namespace {

using namespace reshade::api;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%6.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    wchar_t pid[32] = {}, mine[32];
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"BTSTWEAKS_PID", pid, 32);
    const bool again = !wcscmp(pid, mine);
    SetEnvironmentVariableW(L"BTSTWEAKS_PID", mine);
    g_log = _wfopen((g_exeDir + DT_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded%s", again ? " (again in this process)" : "");
}

// ---------------------------------------------------------------- settings (ReShade.ini)

constexpr int kPeakMin = 400, kPeakMax = 4000, kPaperMin = 80, kPaperMax = 400, kStartMin = 40, kStartMax = 90;
bool g_hdrWanted = true;     // Hdr: the HDR checkbox (the swap chain follows it at the next launch)
bool g_hdrAtLaunch = false;  // what Hdr was when the game started: decides the swap chain
int g_hdrPeak = 1000;        // HdrPeak, nits
int g_hdrPaper = 200;        // HdrPaperWhite, nits
int g_hdrStart = 50;         // HdrHighlightStart, % of SDR white (as encoded)
int g_debugView = 0;         // not saved: 0 off, 1 tint UI, 2 UI alpha
bool g_showWindow = true;    // ShowWindow

// Graphics (applied on the game's main thread; every option starts at the game's own value).
constexpr float kMulMin = 1.0f, kMulMax = 4.0f;
std::atomic<bool> g_gfxOn{true};              // F8 switches every graphics tweak off and on
std::atomic<bool> g_aniso{false};             // Anisotropic: force 16x filtering
std::atomic<float> g_shadowDistMul{1.0f};     // ShadowDistance: times the game's per-scene value
std::atomic<float> g_lodMul{1.0f};            // LodBias: times the game's value
std::atomic<int> g_sunShadow{0};              // SunShadow: 0 game, 4096, 8192 (custom resolution)
std::atomic<int> g_ao{0};                     // Ao: 0 game, 1 better where used, 2 everywhere
std::atomic<int> g_ssr{0};                    // Ssr: 1 full-res reflections where the game uses them
std::atomic<bool> g_taa{false};               // Taa: TAA instead of FXAA
std::atomic<bool> g_shaftsHigh{false};        // SunShaftsHigh
std::mutex g_fxMutex;
std::string g_fxStatus;                       // what the effects code found, for the overlay

void LoadSettings() {
    reshade::get_config_value(nullptr, DT_PRODUCT, "Hdr", g_hdrWanted);
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrPeak", g_hdrPeak);
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrPaperWhite", g_hdrPaper);
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrHighlightStart", g_hdrStart);
    reshade::get_config_value(nullptr, DT_PRODUCT, "ShowWindow", g_showWindow);
    g_hdrPeak = std::clamp(g_hdrPeak, kPeakMin, kPeakMax);
    g_hdrPaper = std::clamp(g_hdrPaper, kPaperMin, kPaperMax);
    g_hdrStart = std::clamp(g_hdrStart, kStartMin, kStartMax);
    g_hdrAtLaunch = g_hdrWanted;
    bool b = false;
    float f = 1;
    int i = 0;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "Anisotropic", b)) g_aniso = b;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "ShadowDistance", f)) g_shadowDistMul = std::clamp(f, kMulMin, kMulMax);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "LodBias", f)) g_lodMul = std::clamp(f, kMulMin, kMulMax);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "SunShadow", i)) g_sunShadow = i == 4096 || i == 8192 ? i : 0;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "Ao", i)) g_ao = std::clamp(i, 0, 2);
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "Ssr", i)) g_ssr = std::clamp(i, 0, 1);  // 2 (everywhere) broke the picture
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "Taa", b)) g_taa = b;
    if (reshade::get_config_value(nullptr, DT_PRODUCT, "SunShaftsHigh", b)) g_shaftsHigh = b;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, DT_PRODUCT, "Hdr", g_hdrWanted);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPeak", g_hdrPeak);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPaperWhite", g_hdrPaper);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrHighlightStart", g_hdrStart);
    reshade::set_config_value(nullptr, DT_PRODUCT, "ShowWindow", g_showWindow);
    reshade::set_config_value(nullptr, DT_PRODUCT, "Anisotropic", g_aniso.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "ShadowDistance", g_shadowDistMul.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "LodBias", g_lodMul.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "SunShadow", g_sunShadow.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "Ao", g_ao.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "Ssr", g_ssr.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "Taa", g_taa.load());
    reshade::set_config_value(nullptr, DT_PRODUCT, "SunShaftsHigh", g_shaftsHigh.load());
}

// ---------------------------------------------------------------- swap chain: 8-bit -> 16-bit float

std::mutex g_bbMutex;
std::set<uint64_t> g_backBuffers;
uint32_t g_bbWidth = 0, g_bbHeight = 0;
std::atomic<bool> g_upgraded{false};  // the swap chain is 16-bit float
std::string g_hdrStatus = "starting";
float g_displayPeak = 0;              // what Windows reports for the game's display
bool g_displayHdr = false;            // Windows HDR is on for the game's display

bool IsBackBuffer(uint64_t h) {
    std::lock_guard<std::mutex> lock(g_bbMutex);
    return h && g_backBuffers.count(h) != 0;
}

bool Is8Bit(format f) {
    switch (f) {
        case format::r8g8b8a8_typeless: case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb:
        case format::b8g8r8a8_typeless: case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
        case format::b8g8r8x8_typeless: case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb:
            return true;
        default:
            return false;
    }
}

// Windows HDR state and peak brightness of the display that holds <hwnd>.
void QueryDisplay(HWND hwnd) {
    g_displayHdr = false;
    g_displayPeak = 0;
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
                DXGI_OUTPUT_DESC1 d = {};
                if (SUCCEEDED(output6->GetDesc1(&d)) && d.Monitor == mon) {
                    g_displayHdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    g_displayPeak = d.MaxLuminance;
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

bool OnCreateSwapchain(device_api api, swapchain_desc& d, void* hwnd) {
    const format f = d.back_buffer.texture.format;
    static int logged = 0;
    const bool log = logged++ < 8;
    if (log)
        Log("swap chain %ux%u format %u, %u buffers, flags 0x%x", d.back_buffer.texture.width, d.back_buffer.texture.height,
            static_cast<uint32_t>(f), d.back_buffer_count, d.present_flags);
    if (api != device_api::d3d11 || !g_hdrAtLaunch || !Is8Bit(f)) return false;
    QueryDisplay(static_cast<HWND>(hwnd));
    if (!g_displayHdr) {
        if (log) Log("Windows HDR is off for this display: swap chain left at 8-bit");
        g_hdrStatus = "Windows HDR is off for this display";
        return false;
    }
    d.back_buffer.texture.format = format::r16g16b16a16_float;
    if (log) Log("swap chain made 16-bit float (display peak %.0f nits)", g_displayPeak);
    return true;
}

void OnInitSwapchain(swapchain* sc, bool) {
    std::lock_guard<std::mutex> lock(g_bbMutex);
    g_backBuffers.clear();
    for (uint32_t i = 0; i < sc->get_back_buffer_count(); ++i) {
        const resource bb = sc->get_back_buffer(i);
        g_backBuffers.insert(bb.handle);
        const resource_desc d = sc->get_device()->get_resource_desc(bb);
        g_bbWidth = d.texture.width, g_bbHeight = d.texture.height;
        g_upgraded = d.texture.format == format::r16g16b16a16_float;
    }
}

// Unity's views on the back buffer ask for 8-bit formats; on the float buffer they must be float.
bool OnCreateResourceView(device*, resource r, resource_usage usage, resource_view_desc& d) {
    if (!g_upgraded || !Is8Bit(d.format) || !IsBackBuffer(r.handle)) return false;
    static int logged = 0;
    if (logged++ < 6)
        Log("back buffer view (%s) format %u -> float", usage == resource_usage::render_target ? "render target" : "shader resource",
            static_cast<uint32_t>(d.format));
    d.format = format::r16g16b16a16_float;
    return true;
}

// ---------------------------------------------------------------- UI layer
//
// The UI's multisampled target is resolved into a plain texture each frame it has UI; the last
// full-screen resolve before present is taken as this frame's UI layer.

std::atomic<uint64_t> g_uiTexture{0};       // resolve destination seen last
std::atomic<bool> g_uiThisFrame{false};     // resolved since the last present
std::atomic<uint64_t> g_uiFrames{0}, g_frames{0};

bool OnResolveUi(device* dev, resource dst) {
    if (!dst.handle) return false;
    const resource_desc d = dev->get_resource_desc(dst);
    if (d.texture.width != g_bbWidth || d.texture.height != g_bbHeight || !Is8Bit(d.texture.format)) return false;
    g_uiTexture = dst.handle;
    g_uiThisFrame = true;
    return true;
}

// ---------------------------------------------------------------- in-game UI: the scene buffer
//
// In play the UI is drawn into the 16-bit scene buffer (H) right before the final tonemap into
// the 8-bit picture (L), which is then blitted to the back buffer (draw logs 2026-10-28):
//   H: one full-screen quad (post-processing), then a re-bind and the UI (quads, text)
//   L: the tonemap quad;  back buffer: the blit quad.
// H and L are learnt from each frame's end (the target drawn before the back buffer is L, the
// one drawn before L is H) and used in the next. H is also the scene's main target, drawn in
// several groups per frame (1, 2, 61, 1, 3 draws in the logs). The UI is the last group, right
// after a single post-processing quad. So H is copied only at the start of a group that follows
// a single quad (two copies a frame), and when L is bound the copy counts only if the group that
// just ended is UI-like (more than one draw, or a draw bigger than one quad). A frame without UI
// ends H with the lone post-processing quad and gets no mask. The HDR pass compares the final H
// with that copy.

std::atomic<bool> g_inPass{false};  // our own commands: the tracking ignores them

struct SceneSplit {
    device* dev = nullptr;
    resource copies[2] = {{0}, {0}};
    resource_view copyViews[2] = {{0}, {0}};
    uint32_t width = 0, height = 0;
    uint64_t h = 0, l = 0;               // learnt last frame
    uint64_t nextH = 0, nextL = 0;       // learnt this frame
    uint64_t bound = 0;                  // render target 0 now
    uint64_t drawnLast = 0, drawnPrev = 0;
    bool frozen = false;                 // blit to the back buffer seen: H is final
    int runDraws = 0;                    // draws into H since the last draw anywhere else (L aside)
    int runCopy = -1;                    // copy of H after the run's first draw, or -1
    int nextCopy = 0;                    // copy to fill next
    int uiCopy = -1;                     // copy before this frame's UI, or -1
    uint64_t hView = 0;                  // H that hViewHandle shows
    resource_view hViewHandle = {0};
} g_split;

std::map<uint64_t, resource_desc> g_descs;  // resource -> desc, for render targets seen

const resource_desc& DescOf(device* dev, uint64_t h) {
    auto it = g_descs.find(h);
    if (it == g_descs.end()) it = g_descs.emplace(h, dev->get_resource_desc(resource{h})).first;
    return it->second;
}

bool IsHalfFloat(format f) { return f == format::r16g16b16a16_typeless || f == format::r16g16b16a16_float; }

void ReleaseSplit() {
    device* dev = g_split.dev;
    if (!dev) return;
    for (int i = 0; i < 2; ++i) {
        if (g_split.copyViews[i].handle) dev->destroy_resource_view(g_split.copyViews[i]);
        if (g_split.copies[i].handle) dev->destroy_resource(g_split.copies[i]);
        g_split.copyViews[i] = {0};
        g_split.copies[i] = {0};
    }
    if (g_split.hViewHandle.handle) dev->destroy_resource_view(g_split.hViewHandle);
    g_split.hViewHandle = {0};
    g_split.hView = 0;
    g_split.width = g_split.height = 0;
}

bool EnsureSplitCopies(device* dev, const resource_desc& hd) {
    if (g_split.dev != dev) {
        ReleaseSplit();
        g_split.dev = dev;
    }
    if (g_split.copies[0].handle && g_split.width == hd.texture.width && g_split.height == hd.texture.height) return true;
    ReleaseSplit();
    g_split.dev = dev;
    resource_desc cd(hd.texture.width, hd.texture.height, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
                     resource_usage::copy_dest | resource_usage::shader_resource);
    for (int i = 0; i < 2; ++i) {
        if (!dev->create_resource(cd, nullptr, resource_usage::shader_resource_pixel, &g_split.copies[i]) ||
            !dev->create_resource_view(g_split.copies[i], resource_usage::shader_resource,
                                       resource_view_desc(format::r16g16b16a16_float), &g_split.copyViews[i])) {
            Log("UI split: couldn't create the scene buffer copies");
            ReleaseSplit();
            return false;
        }
    }
    g_split.width = hd.texture.width, g_split.height = hd.texture.height;
    Log("UI split: scene buffer %ux%u", hd.texture.width, hd.texture.height);
    return true;
}

// In play the frame ends with one run of draws into H: the post-processing composite (one quad),
// then the UI. A copy of H taken after the run's first draw is the picture without UI. Runs with
// a single draw (no UI on screen) give no copy.
void SplitOnBind(command_list* cmd, uint32_t count, const resource_view* rtvs) {
    if (g_inPass) return;
    uint64_t r = 0;
    if (count && rtvs[0].handle) r = cmd->get_device()->get_resource_from_view(rtvs[0]).handle;
    if (r && IsBackBuffer(r) && !g_split.frozen) {
        // The blit: what was drawn before is L, and before that H. H is final now: was its last
        // group the UI? (L itself is also bound at the start of each frame, so it can't mark the end.)
        if (g_split.drawnLast && g_split.drawnPrev && IsHalfFloat(DescOf(cmd->get_device(), g_split.drawnPrev).texture.format))
            g_split.nextL = g_split.drawnLast, g_split.nextH = g_split.drawnPrev;
        g_split.frozen = true;
        g_split.uiCopy = g_split.runDraws >= 2 ? g_split.runCopy : -1;
    }
    g_split.bound = r;
}

void SplitOnDraw(command_list* cmd, uint32_t count) {
    const uint64_t r = g_split.bound;
    if (g_inPass || !r) return;
    if (r != g_split.drawnLast) g_split.drawnPrev = g_split.drawnLast, g_split.drawnLast = r;
    if (g_split.frozen || !g_upgraded || !g_split.h) return;
    if (r != g_split.h) {
        if (r != g_split.l) g_split.runDraws = 0, g_split.runCopy = -1;  // L's tonemap doesn't end the run
        return;
    }
    if (g_split.runDraws == 1) {  // a second draw: keep H as it is after the run's first draw
        device* dev = cmd->get_device();
        if (EnsureSplitCopies(dev, DescOf(dev, r))) {
            g_split.runCopy = g_split.nextCopy;
            g_split.nextCopy ^= 1;
            cmd->copy_resource(resource{r}, g_split.copies[g_split.runCopy]);
        }
    }
    ++g_split.runDraws;
}

// At present: views of H (final) and of the copy before its UI, or false if there's no UI split.
bool SplitViews(device* dev, resource_view* after, resource_view* before) {
    if (!g_split.frozen || g_split.uiCopy < 0 || !g_split.h || g_split.dev != dev) return false;
    if (g_split.hView != g_split.h) {
        if (g_split.hViewHandle.handle) dev->destroy_resource_view(g_split.hViewHandle);
        g_split.hViewHandle = {0};
        g_split.hView = 0;
        if (!dev->create_resource_view(resource{g_split.h}, resource_usage::shader_resource,
                                       resource_view_desc(format::r16g16b16a16_float), &g_split.hViewHandle))
            return false;
        g_split.hView = g_split.h;
    }
    *after = g_split.hViewHandle;
    *before = g_split.copyViews[g_split.uiCopy];
    return true;
}

void SplitOnPresentDone() {
    if (g_split.nextH && (g_split.nextH != g_split.h || g_split.nextL != g_split.l)) {
        static int logged = 0;
        if (logged++ < 6)
            Log("UI split: scene buffer %016llx, picture %016llx", static_cast<unsigned long long>(g_split.nextH),
                static_cast<unsigned long long>(g_split.nextL));
        g_split.h = g_split.nextH, g_split.l = g_split.nextL;
    }
    g_split.nextH = g_split.nextL = 0;
    g_split.drawnLast = g_split.drawnPrev = 0;
    g_split.frozen = false;
    g_split.runDraws = 0;
    g_split.runCopy = g_split.uiCopy = -1;
    g_split.bound = 0;
}

// ---------------------------------------------------------------- the HDR pass

struct Pass {
    device* dev = nullptr;
    pipeline_layout layout = {0};
    pipeline pipe = {0};
    resource copy = {0};
    resource_view copyView = {0};
    uint32_t width = 0, height = 0;
    std::map<uint64_t, resource_view> targets;  // back buffer -> render target view
    uint64_t uiTexture = 0;
    resource_view uiView = {0};
    ID3DDeviceContextState* state = nullptr;    // DirectX 11: the pass's own context state
    bool failed = false;
} g_pass;

effect_runtime* g_runtime = nullptr;
bool g_scRgbSet = false;

void ReleaseTargets() {
    device* dev = g_pass.dev;
    if (!dev) return;
    for (auto& [bb, rtv] : g_pass.targets) dev->destroy_resource_view(rtv);
    g_pass.targets.clear();
    if (g_pass.copyView.handle) dev->destroy_resource_view(g_pass.copyView);
    if (g_pass.copy.handle) dev->destroy_resource(g_pass.copy);
    g_pass.copyView = {0};
    g_pass.copy = {0};
    g_pass.width = g_pass.height = 0;
}

void ReleaseUiView() {
    if (g_pass.dev && g_pass.uiView.handle) g_pass.dev->destroy_resource_view(g_pass.uiView);
    g_pass.uiView = {0};
    g_pass.uiTexture = 0;
}

void ReleasePass() {
    ReleaseTargets();
    ReleaseUiView();
    if (g_pass.dev) {
        if (g_pass.pipe.handle) g_pass.dev->destroy_pipeline(g_pass.pipe);
        if (g_pass.layout.handle) g_pass.dev->destroy_pipeline_layout(g_pass.layout);
    }
    if (g_pass.state) g_pass.state->Release();
    g_pass = {};
}

bool CreatePipeline(device* dev) {
    pipeline_layout_param params[2];
    params[0] = constant_range{0, 0, 0, 8, shader_stage::pixel};
    params[1] = descriptor_range{0, 0, 0, 4, shader_stage::pixel, 1, descriptor_type::shader_resource_view};
    if (!dev->create_pipeline_layout(2, params, &g_pass.layout)) return false;
    shader_desc vs = {};
    vs.code = kHdrVs;
    vs.code_size = sizeof(kHdrVs);
    shader_desc ps = {};
    ps.code = kHdrPs;
    ps.code_size = sizeof(kHdrPs);
    format target = format::r16g16b16a16_float;
    primitive_topology topology = primitive_topology::triangle_list;
    blend_desc blend = {};
    rasterizer_desc raster = {};
    raster.cull_mode = cull_mode::none;
    depth_stencil_desc depth = {};
    depth.depth_enable = false;
    pipeline_subobject subobjects[] = {
        {pipeline_subobject_type::vertex_shader, 1, &vs},
        {pipeline_subobject_type::pixel_shader, 1, &ps},
        {pipeline_subobject_type::render_target_formats, 1, &target},
        {pipeline_subobject_type::primitive_topology, 1, &topology},
        {pipeline_subobject_type::blend_state, 1, &blend},
        {pipeline_subobject_type::rasterizer_state, 1, &raster},
        {pipeline_subobject_type::depth_stencil_state, 1, &depth},
    };
    if (!dev->create_pipeline(g_pass.layout, 7, subobjects, &g_pass.pipe)) return false;
    // DirectX 11: a context state of our own, so the pass doesn't touch Unity's cached bindings.
    auto d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    ID3D11Device1* d3d1 = nullptr;
    if (d3d && SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&d3d1)))) {
        const D3D_FEATURE_LEVEL fl = d3d->GetFeatureLevel();
        d3d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_pass.state);
        d3d1->Release();
    }
    Log("HDR pass ready (own context state: %s)", g_pass.state ? "yes" : "NO");
    return g_pass.state != nullptr;
}

bool EnsureTargets(device* dev, resource bb, const resource_desc& desc, resource_view* rtv) {
    const uint32_t w = desc.texture.width, h = desc.texture.height;
    if (!g_pass.copy.handle || g_pass.width != w || g_pass.height != h) {
        ReleaseTargets();
        resource_desc cd(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
                         resource_usage::copy_dest | resource_usage::shader_resource);
        if (!dev->create_resource(cd, nullptr, resource_usage::shader_resource_pixel, &g_pass.copy)) return false;
        if (!dev->create_resource_view(g_pass.copy, resource_usage::shader_resource,
                                       resource_view_desc(format::r16g16b16a16_float), &g_pass.copyView))
            return false;
        g_pass.width = w, g_pass.height = h;
    }
    auto it = g_pass.targets.find(bb.handle);
    if (it == g_pass.targets.end()) {
        resource_view v = {0};
        if (!dev->create_resource_view(bb, resource_usage::render_target, resource_view_desc(format::r16g16b16a16_float), &v))
            return false;
        it = g_pass.targets.emplace(bb.handle, v).first;
    }
    *rtv = it->second;
    return true;
}

// A view of this frame's UI layer, or a null view.
resource_view UiView(device* dev) {
    const uint64_t tex = g_uiThisFrame ? g_uiTexture.load() : 0;
    if (!tex) return {0};
    if (tex != g_pass.uiTexture) {
        ReleaseUiView();
        if (dev->create_resource_view(resource{tex}, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm),
                                      &g_pass.uiView))
            g_pass.uiTexture = tex;
        else
            Log("UI layer: couldn't create a view of %016llx", static_cast<unsigned long long>(tex));
    }
    return g_pass.uiView;
}

bool RunPass(command_queue* queue, swapchain* sc) {
    device* dev = sc->get_device();
    const resource bb = sc->get_current_back_buffer();
    const resource_desc desc = dev->get_resource_desc(bb);
    if (g_pass.dev != dev) {
        ReleasePass();
        g_pass.dev = dev;
    }
    if (g_pass.failed) return false;
    if (!g_pass.pipe.handle && !CreatePipeline(dev)) {
        Log("HDR pass: couldn't create the pipeline");
        g_pass.failed = true;
        return false;
    }
    resource_view rtv = {0};
    if (!EnsureTargets(dev, bb, desc, &rtv)) {
        Log("HDR pass: couldn't create the frame copy or the back buffer view");
        g_pass.failed = true;
        return false;
    }
    const resource_view ui = UiView(dev);
    resource_view after = {0}, before = {0};
    const bool split = SplitViews(dev, &after, &before);
    command_list* cmd = queue->get_immediate_command_list();
    auto ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3DDeviceContextState* previous = nullptr;
    if (!ctx || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
        Log("HDR pass: no ID3D11DeviceContext1");
        g_pass.failed = true;
        return false;
    }
    g_inPass = true;
    ctx1->SwapDeviceContextState(g_pass.state, &previous);

    cmd->copy_resource(bb, g_pass.copy);
    cmd->bind_pipeline(pipeline_stage::all_graphics, g_pass.pipe);
    const float knee = std::pow(g_hdrStart / 100.0f, 2.2f);
    const float peak = static_cast<float>(g_hdrWanted ? std::max(g_hdrPeak, g_hdrPaper) : g_hdrPaper);
    const float params[8] = {static_cast<float>(g_hdrPaper), peak, knee, ui.handle ? 1.0f : 0.0f,
                             static_cast<float>(g_debugView), split ? 1.0f : 0.0f, 0, 0};
    cmd->push_constants(shader_stage::pixel, g_pass.layout, 0, 0, 8, params);
    const resource_view views[4] = {g_pass.copyView, ui.handle ? ui : g_pass.copyView, split ? after : g_pass.copyView,
                                    split ? before : g_pass.copyView};
    descriptor_table_update update = {};
    update.count = 4;
    update.type = descriptor_type::shader_resource_view;
    update.descriptors = views;
    cmd->push_descriptors(shader_stage::pixel, g_pass.layout, 1, update);
    const viewport vp = {0, 0, static_cast<float>(desc.texture.width), static_cast<float>(desc.texture.height), 0, 1};
    cmd->bind_viewports(0, 1, &vp);
    const rect scissor = {0, 0, static_cast<int32_t>(desc.texture.width), static_cast<int32_t>(desc.texture.height)};
    cmd->bind_scissor_rects(0, 1, &scissor);
    cmd->bind_render_targets_and_depth_stencil(1, &rtv);
    cmd->draw(3, 1, 0, 0);

    ctx1->SwapDeviceContextState(previous, nullptr);
    g_inPass = false;
    if (previous) previous->Release();
    ctx1->Release();
    ++g_frames;
    if (ui.handle || split) ++g_uiFrames;
    return true;
}

bool SetScRgb(swapchain* sc) {
    auto native = reinterpret_cast<IUnknown*>(sc->get_native());
    IDXGISwapChain3* sc3 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    const DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    UINT support = 0;
    const bool ok = SUCCEEDED(sc3->CheckColorSpaceSupport(cs, &support)) &&
                    (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(sc3->SetColorSpace1(cs));
    sc3->Release();
    return ok;
}

// ---------------------------------------------------------------- draw log (diagnostics)

std::mutex g_drawLogMutex;
std::atomic<bool> g_drawLogArmed{false};
bool g_drawLogging = false, g_targetShown = false;
uint64_t g_boundTarget = 0, g_pixelShader = 0;
int g_drawLogLines = 0, g_drawsElsewhere = 0, g_drawSeq = 0;
FILE* g_drawLog = nullptr;

void DrawLog(const char* fmt, ...) {
    if (!g_drawLog || g_drawLogLines >= 3000) return;
    ++g_drawLogLines;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_drawLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_drawLog);
}

void OnBindRenderTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view) {
    SplitOnBind(cmd, count, rtvs);
    if (!g_drawLogging) return;
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    device* dev = cmd->get_device();
    uint64_t target = 0;
    bool bb = false;
    resource_desc d = {};
    format viewFormat = format::unknown;
    if (count && rtvs[0].handle) {
        const resource r = dev->get_resource_from_view(rtvs[0]);
        if (r.handle) {
            d = dev->get_resource_desc(r);
            bb = IsBackBuffer(r.handle);
            viewFormat = dev->get_resource_view_desc(rtvs[0]).format;
            if (bb || (d.texture.width == g_bbWidth && d.texture.height == g_bbHeight)) target = r.handle;
        }
    }
    const bool shown = target != 0;
    if ((shown || g_targetShown) && target != g_boundTarget) {
        if (shown)
            DrawLog("%5d  render target %016llx%s %ux%u format %u, view format %u (%u bound; %d draws elsewhere before)",
                    g_drawSeq, static_cast<unsigned long long>(target), bb ? " = BACK BUFFER" : "", d.texture.width,
                    d.texture.height, static_cast<uint32_t>(d.texture.format), static_cast<uint32_t>(viewFormat), count,
                    g_drawsElsewhere);
        else
            DrawLog("%5d  render target: smaller/other", g_drawSeq);
        g_drawsElsewhere = 0;
    }
    g_boundTarget = target;
    g_targetShown = shown;
}

void OnBindPipeline(command_list*, pipeline_stage stages, pipeline p) {
    if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader) g_pixelShader = p.handle;
}

void LogDraw(const char* kind, uint32_t count, uint32_t instances) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    ++g_drawSeq;
    if (!g_targetShown) {
        ++g_drawsElsewhere;
        return;
    }
    DrawLog("%5d    %s %u x%u  ps %016llx", g_drawSeq, kind, count, instances, static_cast<unsigned long long>(g_pixelShader));
}

bool OnDraw(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    SplitOnDraw(cmd, vertices);
    if (g_drawLogging) LogDraw("draw", vertices, instances);
    return false;
}

bool OnDrawIndexed(command_list* cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    SplitOnDraw(cmd, indices);
    if (g_drawLogging) LogDraw("draw_indexed", indices, instances);
    return false;
}

bool OnResolve(command_list* cmd, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t, uint32_t,
               uint32_t, format) {
    const bool ui = OnResolveUi(cmd->get_device(), dst);
    if (!g_drawLogging) return false;
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    DrawLog("%5d  resolve %016llx -> %016llx%s", g_drawSeq, static_cast<unsigned long long>(src.handle),
            static_cast<unsigned long long>(dst.handle), ui ? " (taken as the UI layer)" : "");
    return false;
}

bool OnCopyResource(command_list*, resource src, resource dst) {
    if (!g_drawLogging) return false;
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    DrawLog("%5d  copy_resource %016llx -> %016llx%s", g_drawSeq, static_cast<unsigned long long>(src.handle),
            static_cast<unsigned long long>(dst.handle), IsBackBuffer(dst.handle) ? " (BACK BUFFER)" : "");
    return false;
}

void DrawLogOnPresent(swapchain* sc) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging) {
        DrawLog("%5d  present (%d draws elsewhere at the end)", g_drawSeq, g_drawsElsewhere);
        fclose(g_drawLog);
        g_drawLog = nullptr;
        g_drawLogging = false;
        Log("draw log: %d lines written to " DT_PRODUCT "-draws.txt", g_drawLogLines);
    }
    if (g_drawLogArmed.exchange(false)) {
        g_drawLog = _wfopen((g_exeDir + DT_PRODUCT_W L"-draws.txt").c_str(), L"a");
        if (g_drawLog) {
            const resource bb = sc->get_current_back_buffer();
            const resource_desc d = sc->get_device()->get_resource_desc(bb);
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(g_drawLog, "\n==== frame %02u:%02u:%02u, back buffer %016llx %ux%u format %u\n", t.wHour, t.wMinute,
                    t.wSecond, static_cast<unsigned long long>(bb.handle), d.texture.width, d.texture.height,
                    static_cast<uint32_t>(d.texture.format));
            g_drawLogging = true;
            g_drawLogLines = g_drawSeq = g_drawsElsewhere = 0;
            g_boundTarget = 0;
            g_targetShown = false;
        }
    }
}

// ---------------------------------------------------------------- present

void OnInitRuntime(effect_runtime* runtime) {
    g_runtime = runtime;
    g_scRgbSet = false;
}

void OnDestroyRuntime(effect_runtime* runtime) {
    if (g_runtime == runtime) g_runtime = nullptr;
}

void OnDestroySwapchain(swapchain* sc, bool) {
    if (g_pass.dev == sc->get_device()) ReleaseTargets();
    g_scRgbSet = false;
}

void OnDestroyDevice(device* dev) {
    if (g_pass.dev == dev) ReleasePass();
    if (g_split.dev == dev) {
        ReleaseSplit();
        g_split = {};
    }
    g_descs.clear();
}

void OnDestroyResource(device* dev, resource r) {
    if (g_pass.dev == dev && r.handle == g_pass.uiTexture) ReleaseUiView();
    if (r.handle == g_uiTexture.load()) g_uiTexture = 0;
    g_descs.erase(r.handle);
    if (r.handle == g_split.h || r.handle == g_split.l) {  // learnt targets gone: learn again
        if (g_split.dev == dev && g_split.hViewHandle.handle && g_split.hView == r.handle) {
            dev->destroy_resource_view(g_split.hViewHandle);
            g_split.hViewHandle = {0};
            g_split.hView = 0;
        }
        g_split.h = g_split.l = 0;
    }
}

// The IL2CPP probe (src/il2cpp_probe.hpp): 0 idle, 1 running, 2 written, 3 failed.
std::atomic<int> g_probeState{0};
bool g_probeReported = false;

void RunProbe() {
    int st = g_probeState;
    if (st >= 2 && g_probeState.compare_exchange_strong(st, 0)) g_probeReported = false;  // run again
    il2cpp_probe::Start(g_exeDir + DT_PRODUCT_W L"-il2cpp.txt", &g_probeState);
}

// ---------------------------------------------------------------- graphics (main thread)

// Each option tracks the game's own value: a value that differs from what the add-on last set
// was set by the game (a new scene, the game's menu) and becomes the new base.
struct Graphics {
    bool resolved = false, ok = false;
    float (*getShadowDistance)() = nullptr;
    void (*setShadowDistance)(float) = nullptr;
    float (*getLodBias)() = nullptr;
    void (*setLodBias)(float) = nullptr;
    int (*getAniso)() = nullptr;
    void (*setAniso)(int) = nullptr;
    void (*setAnisoLimits)(int, int) = nullptr;
    void* (*getSun)() = nullptr;
    int (*getCustomRes)(void*) = nullptr;
    void (*setCustomRes)(void*, int) = nullptr;
    int (*getLightShadowRes)(void*) = nullptr;
    float shadowSet = NAN, lodSet = NAN;
    int anisoSet = -1;
    void* sunSeen = nullptr;
    int sunSet = -1;
} g_gfx;

// The game's values, for the overlay.
std::atomic<float> g_gameShadowDist{-1}, g_gameLod{-1};
std::atomic<int> g_gameAniso{-1}, g_gameSunRes{-1}, g_gameSunCustom{-1};
std::atomic<bool> g_sunFound{false};

void ResolveGraphics() {
    using il2cpp_main::Icall;
    g_gfx.resolved = true;
    auto get = [](auto& f, const char* name) {
        f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(Icall(name));
        if (!f) Log("graphics: Unity call %s not found", name);
        return f != nullptr;
    };
    bool ok = true;
    ok &= get(g_gfx.getShadowDistance, "UnityEngine.QualitySettings::get_shadowDistance()");
    ok &= get(g_gfx.setShadowDistance, "UnityEngine.QualitySettings::set_shadowDistance(System.Single)");
    ok &= get(g_gfx.getLodBias, "UnityEngine.QualitySettings::get_lodBias()");
    ok &= get(g_gfx.setLodBias, "UnityEngine.QualitySettings::set_lodBias(System.Single)");
    ok &= get(g_gfx.getAniso, "UnityEngine.QualitySettings::get_anisotropicFiltering()");
    ok &= get(g_gfx.setAniso, "UnityEngine.QualitySettings::set_anisotropicFiltering(UnityEngine.AnisotropicFiltering)");
    get(g_gfx.setAnisoLimits, "UnityEngine.Texture::SetGlobalAnisotropicFilteringLimits(System.Int32,System.Int32)");
    ok &= get(g_gfx.getSun, "UnityEngine.RenderSettings::get_sun()");
    ok &= get(g_gfx.getCustomRes, "UnityEngine.Light::get_shadowCustomResolution()");
    ok &= get(g_gfx.setCustomRes, "UnityEngine.Light::set_shadowCustomResolution(System.Int32)");
    get(g_gfx.getLightShadowRes, "UnityEngine.Light::get_shadowResolution()");
    g_gfx.ok = ok;
    Log("graphics: Unity calls %s", ok ? "found" : "MISSING: graphics options off");
}

void ApplyFloat(float (*get)(), void (*set)(float), float& lastSet, std::atomic<float>& game, float mul) {
    const float v = get();
    if (!(std::fabs(v - lastSet) < 1e-3f)) game = v;  // set by the game
    const float target = game * mul;
    if (std::fabs(v - target) >= 1e-3f) set(target);
    lastSet = target;
}

void ApplyEffects() {
    static bool failed = false;
    if (failed) return;
    const effects::Wanted w = {g_gfxOn, g_ao, g_ssr, g_taa, g_shaftsHigh};
    std::string status;
    if (!effects::Update(w, &status)) {
        failed = true;
        Log("effects: %s", status.c_str());
    }
    std::lock_guard<std::mutex> lock(g_fxMutex);
    if (status != g_fxStatus) {
        if (!failed) Log("effects: %s", status.c_str());
        g_fxStatus = status;
    }
}

void ApplyGraphics() {
    ApplyEffects();
    if (!g_gfx.resolved) ResolveGraphics();
    if (!g_gfx.ok) return;
    const bool on = g_gfxOn;
    ApplyFloat(g_gfx.getShadowDistance, g_gfx.setShadowDistance, g_gfx.shadowSet, g_gameShadowDist,
               on ? g_shadowDistMul.load() : 1.0f);
    ApplyFloat(g_gfx.getLodBias, g_gfx.setLodBias, g_gfx.lodSet, g_gameLod, on ? g_lodMul.load() : 1.0f);

    // Anisotropic filtering: the game leaves it off; ForceEnable with a 16x minimum.
    const int a = g_gfx.getAniso();
    if (a != g_gfx.anisoSet) g_gameAniso = a;
    const int aTarget = on && g_aniso ? 2 : g_gameAniso.load();
    if (a != aTarget) {
        g_gfx.setAniso(aTarget);
        if (g_gfx.setAnisoLimits) {
            if (aTarget == 2 && g_gameAniso != 2) g_gfx.setAnisoLimits(16, 16);
            else g_gfx.setAnisoLimits(9, 16);  // Unity's defaults
        }
    }
    g_gfx.anisoSet = aTarget;

    // The sun's shadow map: a custom resolution on RenderSettings.sun (the game sets only the
    // resolution tier, so the custom value, which takes priority, stays the add-on's).
    void* sun = g_gfx.getSun();
    g_sunFound = sun != nullptr;
    if (!sun) return;
    if (sun != g_gfx.sunSeen) {  // a new scene's sun
        g_gfx.sunSeen = sun;
        g_gfx.sunSet = -1;
    }
    const int c = g_gfx.getCustomRes(sun);
    if (c != g_gfx.sunSet) g_gameSunCustom = c;
    if (g_gfx.getLightShadowRes) g_gameSunRes = g_gfx.getLightShadowRes(sun);
    const int cTarget = on && g_sunShadow ? g_sunShadow.load() : g_gameSunCustom.load();
    if (c != cTarget) g_gfx.setCustomRes(sun, cTarget);
    g_gfx.sunSet = cTarget;

    // Log what Unity reports after a change (read back, not what was asked for).
    static std::string lastLogged;
    char b[256];
    snprintf(b, sizeof b, "shadow distance %g (game %g), LOD bias %g (game %g), filtering mode %d (game %d), sun custom %d (game %d)",
             g_gfx.getShadowDistance(), g_gameShadowDist.load(), g_gfx.getLodBias(), g_gameLod.load(), g_gfx.getAniso(),
             g_gameAniso.load(), g_gfx.getCustomRes(sun), g_gameSunCustom.load());
    if (lastLogged != b) {
        lastLogged = b;
        Log("graphics applied: %s", b);
    }
}

// Unity's quality settings, read on the main thread through Unity's internal calls.
void LogQuality() {
    using namespace il2cpp_main;
    struct Item { const char* name; char kind; };  // f float, i int/enum, b bool
    static const Item items[] = {
        {"GetQualityLevel", 'i'}, {"get_shadows", 'i'}, {"get_shadowResolution", 'i'}, {"get_shadowProjection", 'i'},
        {"get_shadowDistance", 'f'}, {"get_shadowCascades", 'i'}, {"get_shadowCascade2Split", 'f'},
        {"get_shadowNearPlaneOffset", 'f'}, {"get_shadowmaskMode", 'i'}, {"get_lodBias", 'f'},
        {"get_maximumLODLevel", 'i'}, {"get_anisotropicFiltering", 'i'}, {"get_masterTextureLimit", 'i'},
        {"get_pixelLightCount", 'i'}, {"get_antiAliasing", 'i'}, {"get_softParticles", 'b'},
        {"get_softVegetation", 'b'}, {"get_realtimeReflectionProbes", 'b'}, {"get_billboardsFaceCameraPosition", 'b'},
        {"get_particleRaycastBudget", 'i'}, {"get_skinWeights", 'i'}, {"get_streamingMipmapsActive", 'b'},
        {"get_vSyncCount", 'i'}, {"get_asyncUploadTimeSlice", 'i'}, {"get_asyncUploadBufferSize", 'i'}};
    std::string line = "Unity quality (hook calls: " + Report() + "):";
    for (const Item& it : items) {
        const std::string full = std::string("UnityEngine.QualitySettings::") + it.name + "()";
        void* f = Icall(full.c_str());
        char b[96];
        const char* shortName = strncmp(it.name, "get_", 4) ? it.name : it.name + 4;
        if (!f) snprintf(b, sizeof b, " %s=?", shortName);
        else if (it.kind == 'f') snprintf(b, sizeof b, " %s=%g", shortName, reinterpret_cast<float (*)()>(f)());
        else if (it.kind == 'b') snprintf(b, sizeof b, " %s=%d", shortName, reinterpret_cast<bool (*)()>(f)() ? 1 : 0);
        else snprintf(b, sizeof b, " %s=%d", shortName, reinterpret_cast<int (*)()>(f)());
        line += b;
    }
    Log("%s", line.c_str());
    Log("sun light: %s, shadow resolution tier %d, custom %d (game's custom %d)", g_sunFound ? "found" : "none",
        g_gameSunRes.load(), g_gfx.ok && g_sunFound ? g_gfx.getCustomRes(g_gfx.getSun()) : -1, g_gameSunCustom.load());
}

// Camera.main's settings, for the live dump.
void LiveCamera(FILE* f) {
    using il2cpp_main::Icall;
    auto mainCam = reinterpret_cast<void* (*)()>(Icall("UnityEngine.Camera::get_main()"));
    auto count = reinterpret_cast<int (*)()>(Icall("UnityEngine.Camera::get_allCamerasCount()"));
    void* cam = mainCam ? mainCam() : nullptr;
    fprintf(f, "cameras: %d, Camera.main %s\n", count ? count() : -1, cam ? "found" : "none");
    if (!cam) return;
    struct Item { const char* name; char kind; };
    static const Item items[] = {{"get_renderingPath", 'i'}, {"get_actualRenderingPath", 'i'}, {"get_nearClipPlane", 'f'},
                                 {"get_farClipPlane", 'f'}, {"get_fieldOfView", 'f'}, {"get_allowMSAA", 'b'},
                                 {"get_allowHDR", 'b'}, {"get_useOcclusionCulling", 'b'}, {"get_layerCullSpherical", 'b'},
                                 {"get_depthTextureMode", 'i'}, {"get_cullingMask", 'i'}};
    for (const Item& it : items) {
        void* fn = Icall((std::string("UnityEngine.Camera::") + it.name + "()").c_str());
        if (!fn) fprintf(f, "  %s: not found\n", it.name + 4);
        else if (it.kind == 'f') fprintf(f, "  %s = %g\n", it.name + 4, reinterpret_cast<float (*)(void*)>(fn)(cam));
        else if (it.kind == 'b') fprintf(f, "  %s = %d\n", it.name + 4, reinterpret_cast<bool (*)(void*)>(fn)(cam) ? 1 : 0);
        else fprintf(f, "  %s = %d\n", it.name + 4, reinterpret_cast<int (*)(void*)>(fn)(cam));
    }
    if (auto lcd = reinterpret_cast<void* (*)(void*)>(Icall("UnityEngine.Camera::get_layerCullDistances()"))) {
        const char* arr = static_cast<const char*>(lcd(cam));
        if (arr) {
            const float* v = reinterpret_cast<const float*>(arr + 0x20);
            fprintf(f, "  layerCullDistances =");
            for (int i = 0; i < 32; ++i) fprintf(f, " %g", v[i]);
            fprintf(f, "\n");
        }
    }
}

void LiveDump() {
    static const char* const classes[] = {
        "PostProcessingBehaviour", "ScreenSpaceReflection", "SunShafts", "GlobalFog", "CameraMotionBlur", "TiltShift",
        "DepthOfFieldDeprecated", "MirrorReflection", "CrossPlatformLightQuality", "CameraUpscaler", "SceneGraphicsSettings"};
    const bool ok = il2cpp_probe::RunLive(g_exeDir + DT_PRODUCT_W L"-live.txt", classes, sizeof classes / sizeof classes[0],
                                          LiveCamera);
    Log(ok ? "live settings written to " DT_PRODUCT "-live.txt" : "live settings: couldn't write the file");
}

void ProbeOnPresent() {
    ++il2cpp_main::g_state.frame;
    static const ULONGLONG start = GetTickCount64();
    static bool installTried = false;
    if (!installTried && GetTickCount64() - start > 3000) {
        installTried = true;
        il2cpp_main::g_state.everyFrame.push_back(ApplyGraphics);
        Log("main thread hooks: %s", il2cpp_main::Install().c_str());
    }
    static bool qualityLogged = false;
    if (!qualityLogged && GetTickCount64() - start > 45000) {
        qualityLogged = true;
        Log("main thread hooks after 45 s: %s", il2cpp_main::Report().c_str());
        il2cpp_main::Post(LogQuality);
    }
    const int st = g_probeState;
    if (st >= 2 && !g_probeReported) {
        Log(st == 2 ? "IL2CPP probe written to " DT_PRODUCT "-il2cpp.txt" : "IL2CPP probe: couldn't write its file");
        g_probeReported = true;
    }
}

void OnPresent(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (g_runtime && g_runtime->is_key_pressed(VK_F8)) {
        g_gfxOn = !g_gfxOn;
        Log("graphics tweaks %s (F8)", g_gfxOn ? "on" : "off");
    }
    ProbeOnPresent();
    DrawLogOnPresent(sc);
    if (g_upgraded) {
        const bool ran = RunPass(queue, sc);
        if (ran && !g_scRgbSet) {
            const bool ok = SetScRgb(sc);
            if (g_runtime) g_runtime->set_color_space(color_space::extended_srgb_linear);
            Log("HDR on: swap chain scRGB %s (paper white %d, peak %d nits, highlights from %d%%)", ok ? "set" : "REFUSED",
                g_hdrPaper, g_hdrPeak, g_hdrStart);
            g_hdrStatus = ok ? "on (scRGB, 16-bit float)" : "the display refused scRGB";
            g_scRgbSet = true;
        }
        if (!ran) g_hdrStatus = "HDR pass failed (see the log)";
    } else if (g_hdrStatus == "starting") {
        g_hdrStatus = g_hdrAtLaunch ? "the swap chain couldn't be made 16-bit (see the log)" : "off";
    }
    g_uiThisFrame = false;
    SplitOnPresentDone();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void DrawPanel() {
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    if (g_upgraded)
        ImGui::TextColored(kGood, "  HDR %s", g_hdrWanted ? "on" : "off (SDR look)");
    else
        ImGui::TextColored(kWarn, "  HDR: %s", g_hdrStatus.c_str());

    bool save = false;
    ImGui::SeparatorText("HDR");
    save |= ImGui::Checkbox("HDR", &g_hdrWanted);
    Help("The game's menu has no HDR option. The game renders as usual, with its own colour\n"
         "grade, and the add-on turns the finished picture into HDR, brightening only the\n"
         "brightest parts (sun, lamps, reflections) up to the peak you set below. Menus and\n"
         "subtitles stay at paper white. Windows HDR must be on.\n\n"
         "The game's screen image is switched to 16-bit when it starts, so turning HDR on takes\n"
         "a restart of the game; turning it off here keeps the picture at SDR brightness.");
    if (g_hdrWanted != g_hdrAtLaunch && !(g_upgraded && !g_hdrWanted))
        ImGui::TextColored(kWarn, "Restart the game for this to take effect.");

    ImGui::BeginDisabled(!g_upgraded);
    ImGui::SetNextItemWidth(ControlWidth());
    save |= ImGui::SliderInt("Peak brightness", &g_hdrPeak, kPeakMin, kPeakMax, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    Help("How bright the brightest parts of the picture get: the sun, lamps, reflections.\n"
         "Set it to your display's peak brightness (or a little below, for a softer look).\n"
         "Ctrl+click to type a value.");
    if (g_displayPeak > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use display's")) {
            g_hdrPeak = std::clamp(static_cast<int>(g_displayPeak + 0.5f), kPeakMin, kPeakMax);
            save = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Windows reports %.0f nits for this display.", g_displayPeak);
    }
    ImGui::SetNextItemWidth(ControlWidth());
    save |= ImGui::SliderInt("Paper white", &g_hdrPaper, kPaperMin, kPaperMax, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    Help("How bright everything else is: walls, faces, the menus and subtitles. 200 is a\n"
         "common choice; raise it in a bright room, lower it in a dark one.");
    ImGui::SetNextItemWidth(ControlWidth());
    save |= ImGui::SliderInt("Highlights start at", &g_hdrStart, kStartMin, kStartMax, "%d%% of white",
                             ImGuiSliderFlags_AlwaysClamp);
    Help("Where the picture starts getting brighter than SDR. Below this, everything looks\n"
         "exactly as the game graded it; above it, brightness rises towards the peak. Lower is\n"
         "punchier; higher stays closer to SDR. Default 50%.");
    ImGui::EndDisabled();
    if (g_hdrPeak < g_hdrPaper) ImGui::TextColored(kWarn, "Peak is below paper white: highlights aren't stretched.");

    ImGui::SeparatorText("Graphics");
    bool gfxOn = g_gfxOn;
    if (ImGui::Checkbox("Graphics tweaks (F8)", &gfxOn)) g_gfxOn = gfxOn;
    Help("Switches every graphics option below off and on, to compare. Not saved: they're on\n"
         "each time the game starts.");
    if (!il2cpp_main::g_state.calls) ImGui::TextColored(kWarn, "Waiting for the game's main thread...");
    bool aniso = g_aniso;
    if (ImGui::Checkbox("16x texture filtering", &aniso)) g_aniso = aniso, save = true;
    Help("The game leaves anisotropic filtering off, so ground, roads and walls blur at an angle.\n"
         "This forces 16x on every texture. Costs very little.");
    float sd = g_shadowDistMul;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderFloat("Shadow distance", &sd, kMulMin, kMulMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp))
        g_shadowDistMul = sd, save = true;
    Help("How far shadows are drawn, as a multiple of the game's value (it sets it per scene,\n"
         "60 in the first scene). Farther shadows are spread over the same shadow\n"
         "map, so raise the sun's shadow map with it.");
    float lod = g_lodMul;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderFloat("Detail distance", &lod, kMulMin, kMulMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp))
        g_lodMul = lod, save = true;
    Help("Unity's LOD bias, as a multiple of the game's value: models switch to their simpler\n"
         "versions farther away.");
    static const int sunValues[] = {0, 4096, 8192};
    static const char* sunNames[] = {"Game", "4096", "8192"};
    int sunIndex = g_sunShadow == 8192 ? 2 : g_sunShadow == 4096 ? 1 : 0;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Sun shadow map", &sunIndex, sunNames, 3)) g_sunShadow = sunValues[sunIndex], save = true;
    Help("The resolution of the sun's shadow map (shared by its 4 cascades). Sharper shadows,\n"
         "especially with a longer shadow distance. 8192 costs more video memory and time.");
    if (g_gameShadowDist >= 0)
        ImGui::TextDisabled("Game: shadow distance %.0f, LOD bias %.2g, filtering %s, sun %s", g_gameShadowDist.load(),
                            g_gameLod.load(), g_gameAniso == 0 ? "off" : g_gameAniso == 1 ? "per texture" : "forced",
                            !g_sunFound ? "none" : g_gameSunCustom > 0 ? "custom" : "tier");

    ImGui::SeparatorText("Effects");
    static const char* useNames[] = {"Game", "Better where used", "Everywhere"};
    int ao = g_ao;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Ambient occlusion", &ao, useNames, 3)) g_ao = ao, save = true;
    Help("Contact shadows in corners and creases.\n"
         "Better where used: where a scene has it, 16 samples at full resolution (the game uses\n"
         "10 at half resolution). Everywhere: also turns it on in scenes made without it, which\n"
         "changes their look.");
    bool ssr = g_ssr != 0;
    if (ImGui::Checkbox("Sharper reflections", &ssr)) g_ssr = ssr ? 1 : 0, save = true;
    Help("In scenes with screen-space reflections (floors, cars, water), draws them at full\n"
         "resolution instead of the game's low setting. Scenes made without them stay without:\n"
         "turning them on there breaks the picture.");
    bool taa = g_taa;
    if (ImGui::Checkbox("Temporal anti-aliasing", &taa)) g_taa = taa, save = true;
    Help("Uses the TAA built into the game's post-processing instead of its FXAA: steadier edges\n"
         "and less shimmer on fine detail, a little softer.");
    bool shafts = g_shaftsHigh;
    if (ImGui::Checkbox("High-resolution sun shafts", &shafts)) g_shaftsHigh = shafts, save = true;
    Help("Draws the light shafts at full quality: the game leaves them at its low setting.");
    {
        std::lock_guard<std::mutex> lock(g_fxMutex);
        if (!g_fxStatus.empty()) ImGui::TextDisabled("Effects: %s", g_fxStatus.c_str());
    }

    static bool unlogged = false;
    if (save) {
        SaveSettings();
        unlogged = true;
    }
    if (unlogged && !ImGui::IsAnyItemActive()) {
        Log("settings: HDR %s, peak %d, paper white %d nits, highlights from %d%%; 16x filtering %d, shadow distance %.2fx, "
            "detail distance %.2fx, sun shadow map %d; AO %d, SSR %d, TAA %d, sun shafts high %d",
            g_hdrWanted ? "on" : "off", g_hdrPeak, g_hdrPaper, g_hdrStart, g_aniso ? 1 : 0, g_shadowDistMul.load(),
            g_lodMul.load(), g_sunShadow.load(), g_ao.load(), g_ssr.load(), g_taa ? 1 : 0, g_shaftsHigh ? 1 : 0);
        unlogged = false;
    }

    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::TextDisabled("Swap chain: %s; display: Windows HDR %s, %.0f nits", g_upgraded ? "16-bit float" : "8-bit",
                            g_displayHdr ? "on" : "off/unknown", g_displayPeak);
        ImGui::TextDisabled("UI kept at paper white in %llu of %llu frames", static_cast<unsigned long long>(g_uiFrames.load()),
                            static_cast<unsigned long long>(g_frames.load()));
        ImGui::SetNextItemWidth(ControlWidth());
        const char* views[] = {"Off", "Tint what counts as UI", "Show what counts as UI"};
        ImGui::Combo("HDR debug view", &g_debugView, views, 3);
        Help("For diagnosing HDR. Tint: pixels kept at paper white as UI turn magenta.\n"
             "Show: what counts as UI in white.");
        if (ImGui::Button("Log one frame's drawing")) g_drawLogArmed = true;
        Help("Writes the next frame's render target changes and draw calls on screen-sized targets\n"
             "to " DT_PRODUCT "-draws.txt next to the game's exe.");
        ImGui::TextDisabled("Main thread: %llu frames (%s)", static_cast<unsigned long long>(il2cpp_main::g_state.calls.load()),
                            il2cpp_main::Report().c_str());
        if (ImGui::Button("Log Unity's quality settings")) il2cpp_main::Post(LogQuality);
        if (ImGui::Button("Write the camera's and effects' settings")) il2cpp_main::Post(LiveDump);
        Help("Read-only: writes the main camera and the live post-processing and effect objects\n"
             "with their settings to " DT_PRODUCT "-live.txt next to the game's exe.");
        if (ImGui::Button("Write the game's settings classes")) RunProbe();
        Help("Read-only: writes the game's settings classes, their fields and current values\n"
             "to " DT_PRODUCT "-il2cpp.txt next to the game's exe.");
        ImGui::TextDisabled("Log: " DT_PRODUCT ".log next to the game's exe");
    }
}

// The add-on's own floating window, shown while ReShade's overlay is open.
std::atomic<bool> g_overlayOpen{false};

bool OnOpenOverlay(effect_runtime*, bool open, input_source) {
    g_overlayOpen = open;
    return false;
}

void DrawWindow(effect_runtime*) {
    if (!g_overlayOpen || !g_showWindow) return;
    const float fs = ImGui::GetFontSize();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(fs * 34, fs * 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(display.x - fs * 36, fs * 2), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin(DT_PRODUCT "###" DT_PRODUCT "Window", &open)) DrawPanel();
    ImGui::End();
    if (!open) {
        g_showWindow = false;
        SaveSettings();
    }
}

void DrawSettings(effect_runtime*) {
    if (ImGui::Checkbox("Show the " DT_PRODUCT " window", &g_showWindow)) SaveSettings();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
            LoadSettings();
            reshade::register_event<reshade::addon_event::create_swapchain>(&OnCreateSwapchain);
            reshade::register_event<reshade::addon_event::init_swapchain>(&OnInitSwapchain);
            reshade::register_event<reshade::addon_event::create_resource_view>(&OnCreateResourceView);
            reshade::register_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchain);
            reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
            reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
            reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
            reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OnOpenOverlay);
            reshade::register_event<reshade::addon_event::reshade_overlay>(&DrawWindow);
            reshade::register_overlay(nullptr, &DrawSettings);
            break;
        case DLL_PROCESS_DETACH:
            il2cpp_main::Uninstall();
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
