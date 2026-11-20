// OpenXR side of AI Limit VR. See AIXR.h for the threading.
// Ported from Ori WotW VR's WotwXR.cpp and Ori VR's OriVRNative.cpp (same author, MIT): runtime and add-on logging,
// the -32 retry without add-ons, own XR thread and D3D11 device, ring of shared textures, Picture Auto.
// New here: the eyes are the projection layer itself (3D world, not a diorama stage); HUD panel Fixed or Follows head.
#include "AIXR.h"
#include "MenuDraw.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace aixr {

static void NoLog(const char*, ...) {}
static void (*Log)(const char*, ...) = NoLog;
void SetLogger(void (*log)(const char*, ...)) { Log = log ? log : NoLog; }

// ------------------------------------------------------------------------------------------------ settings and state
static std::atomic<int> g_state{Off};
static std::atomic<bool> g_slotsReady{false};
static std::atomic<int> g_lastError{0};
static std::atomic<int> g_layerInUse{-1};
static int g_eyeW = 0, g_eyeH = 0, g_picture = 0, g_hudW = 1920, g_hudH = 1080;
static float g_resolution = 1;
static std::mutex g_hudMx;
static Panel g_hud;
static char g_runtime[XR_MAX_RUNTIME_NAME_SIZE] = "", g_headset[XR_MAX_SYSTEM_NAME_SIZE] = "";
static std::atomic<int> g_recW{0}, g_recH{0};
static std::thread g_thread;
static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_wantRecenter{true};

void Configure(int picture, float resolution, int hudW, int hudH) { g_picture = picture; g_resolution = resolution; g_hudW = hudW; g_hudH = hudH; }
int EyeWidth() { return g_slotsReady ? g_eyeW : 0; }
int EyeHeight() { return g_slotsReady ? g_eyeH : 0; }
void SetHud(const Panel& p) { std::lock_guard<std::mutex> lk(g_hudMx); bool replace = p.mode != g_hud.mode || p.x != g_hud.x || p.y != g_hud.y || p.distance != g_hud.distance; g_hud = p; (void)replace; }
int GetState() { return g_state; }
const char* RuntimeName() { return g_runtime; }
const char* HeadsetName() { return g_headset; }
int RecommendedWidth() { return g_recW; }
int RecommendedHeight() { return g_recH; }
int LayerInUse() { return g_layerInUse; }
int LastError() { return g_lastError; }
void Recenter() { g_wantRecenter = true; }

static std::mutex g_viewsMx;
static Views g_views;
bool GetViews(Views* out) { std::lock_guard<std::mutex> lk(g_viewsMx); *out = g_views; return out->valid; }

// ------------------------------------------------------------------------------------------------ stats
static std::mutex g_statMx;
static Stats g_stats{};
static double g_waitSum = 0, g_endSum = 0, g_gpuSum = 0; static uint32_t g_timed = 0;
void TakeStats(Stats* out) {
    std::lock_guard<std::mutex> lk(g_statMx);
    *out = g_stats;
    out->waitMsAvg = g_timed ? g_waitSum / g_timed : 0;
    out->endMsAvg = g_timed ? g_endSum / g_timed : 0;
    out->gpuMsAvg = g_stats.gpuSamples ? g_gpuSum / g_stats.gpuSamples : 0;
    g_stats = Stats{}; g_waitSum = g_endSum = g_gpuSum = 0; g_timed = 0;
}
static LARGE_INTEGER g_qpf;
static double Ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf.QuadPart; }

// ------------------------------------------------------------------------------------------------ OpenXR functions
static HMODULE g_loader;
static PFN_xrGetInstanceProcAddr g_gipa;
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = 0;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
static int64_t g_format = 0;
template <class T> static bool GetFn(const char* name, T& fn) {
    fn = nullptr;
    if (XR_FAILED(g_gipa(g_inst, name, (PFN_xrVoidFunction*)&fn)) || !fn) { Log("XR missing function %s", name); return false; }
    return true;
}
#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { Log("XR %s failed: %d", #x, (int)r_); g_lastError = (int)r_; return false; } } while (0)
static PFN_xrPollEvent p_poll; static PFN_xrBeginSession p_beginSession; static PFN_xrEndSession p_endSession;
static PFN_xrWaitFrame p_waitFrame; static PFN_xrBeginFrame p_beginFrame; static PFN_xrEndFrame p_endFrame;
static PFN_xrLocateViews p_locate; static PFN_xrLocateSpace p_locateSpace; static PFN_xrCreateReferenceSpace p_createSpace;
static PFN_xrCreateSwapchain p_createSwap; static PFN_xrEnumerateSwapchainImages p_enumImages; static PFN_xrDestroySwapchain p_destroySwap;
static PFN_xrAcquireSwapchainImage p_acquire; static PFN_xrWaitSwapchainImage p_waitImage; static PFN_xrReleaseSwapchainImage p_release;
static PFN_xrDestroySession p_destroySession; static PFN_xrDestroyInstance p_destroyInstance;

// ------------------------------------------------------------------------------------------------ D3D11 (XR side)
static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx;
static ID3D11VertexShader* g_vs; static ID3D11PixelShader* g_ps; static ID3D11Buffer* g_cb;
static ID3D11SamplerState* g_linear; static ID3D11RasterizerState* g_noCull; static ID3D11BlendState* g_alphaBlend;
struct Swap { XrSwapchain handle = XR_NULL_HANDLE; std::vector<XrSwapchainImageD3D11KHR> images; std::vector<ID3D11RenderTargetView*> rtv; int w = 0, h = 0; };
static Swap g_proj[2], g_hudSwap, g_menuSwap;

// Ring of shared frames (left eye, right eye, HUD). Key 0: free for the game side to write. Key 1: written, owned by the XR side.
static const int kSlots = 4;
static const int kTex = 3;
struct Slot {
    ID3D11Texture2D* xrTex[kTex] = {}; IDXGIKeyedMutex* xrMx[kTex] = {}; ID3D11ShaderResourceView* srv[kTex] = {};
    HANDLE share[kTex] = {};
    ID3D11Texture2D* gameTex[kTex] = {}; IDXGIKeyedMutex* gameMx[kTex] = {};
    std::atomic<uint64_t> seq{0};   // frame order written by the game side, 0 = never written
    Views views;                    // the views this pair was rendered with
    bool hasHud = false;
    std::atomic<bool> xrHolds{false};
};
static Slot g_slots[kSlots];
static std::atomic<uint64_t> g_seq{0};

static bool CreateDeviceForRuntime(const LUID& want) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
    IDXGIAdapter1* chosen = nullptr; IDXGIAdapter1* a = nullptr;
    bool zero = want.HighPart == 0 && want.LowPart == 0;  // RigB's null driver reports zero
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        bool match = !zero && d.AdapterLuid.HighPart == want.HighPart && d.AdapterLuid.LowPart == want.LowPart;
        if (!chosen && (match || (zero && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)))) { chosen = a; Log("XR device on %ls (%s)", d.Description, match ? "runtime's adapter" : "first adapter, runtime gave no adapter"); continue; }
        a->Release();
    }
    f->Release();
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { Log("XR device creation failed %08lx", (unsigned long)hr); return false; }
    return true;
}

static const char* kRectShader = R"(
Texture2D src : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Rect : register(b0) { float4 corner[4]; float4 opt; };
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2(id & 1, id >> 1);
    if (opt.x > 0.5) uv.y = 1 - uv.y;   // Unity's render textures are stored bottom row first
    pos = corner[id];
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return src.Sample(linearClamp, uv);
}
)";

static bool CreateRectPass() {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { Log("XR no shader compiler (d3dcompiler_47.dll)"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    size_t n = strlen(kRectShader);
    if (FAILED(compile(kRectShader, n, "rect", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kRectShader, n, "rect", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("XR rect shader error %s", err ? (const char*)err->GetBufferPointer() : "?"); return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 80; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&cb, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_linear);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_noCull);
    D3D11_BLEND_DESC bd{}; auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask = 15;
    g_dev->CreateBlendState(&bd, &g_alphaBlend);
    return g_vs && g_ps && g_cb && g_linear && g_noCull;
}

static bool MakeSwap(Swap& s, int w, int h, uint64_t usage) {
    XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    c.usageFlags = usage; c.format = g_format; c.sampleCount = 1; c.width = w; c.height = h;
    c.faceCount = c.arraySize = c.mipCount = 1;
    XRCHECK(p_createSwap(g_sess, &c, &s.handle));
    uint32_t ic = 0;
    XRCHECK(p_enumImages(s.handle, 0, &ic, nullptr));
    s.images.assign(ic, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRCHECK(p_enumImages(s.handle, ic, &ic, (XrSwapchainImageBaseHeader*)s.images.data()));
    s.rtv.assign(ic, nullptr); s.w = w; s.h = h;
    return true;
}

static bool CreateSlots() {
    bool srgb = g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (auto& s : g_slots) for (int e = 0; e < kTex; e++) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = e < 2 ? g_eyeW : g_hudW; d.Height = e < 2 ? g_eyeH : g_hudH; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &s.xrTex[e]))) { Log("XR slot texture failed"); return false; }
        s.xrTex[e]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.xrMx[e]);
        IDXGIResource* r = nullptr; s.xrTex[e]->QueryInterface(__uuidof(IDXGIResource), (void**)&r);
        if (r) { r->GetSharedHandle(&s.share[e]); r->Release(); }
        // The game renders sRGB-encoded colour into its textures: read them as sRGB so a UNORM_SRGB target round-trips.
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(s.xrTex[e], &sv, &s.srv[e]);
        if (!s.xrMx[e] || !s.share[e] || !s.srv[e]) { Log("XR slot setup failed"); return false; }
    }
    g_slotsReady = true;
    Log("XR ring of %d shared frames ready: eyes %dx%d, HUD %dx%d", kSlots, g_eyeW, g_eyeH, g_hudW, g_hudH);
    return true;
}

// ------------------------------------------------------------------------------------------------ add-ons and runtime
static std::string ReadText(const char* path) {
    std::string t; FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
    return t;
}
static std::string JsonString(const std::string& text, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = text.find(k); if (at == std::string::npos) return "";
    at = text.find(':', at + k.size()); if (at == std::string::npos) return "";
    at = text.find('"', at); if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + 1; i < text.size(); i++) { char c = text[i]; if (c == '\\' && i + 1 < text.size()) { out += text[++i]; continue; } if (c == '"') return out; out += c; }
    return "";
}
static std::vector<std::string> g_offSwitches; static std::string g_offNames, g_stuckNames;
static void ListLayers(HKEY root, const char* rootName) {
    HKEY key;
    if (RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        char manifest[1024]; DWORD nl = sizeof manifest, type = 0, data = 0, dl = sizeof data;
        LONG r = RegEnumValueA(key, i, manifest, &nl, nullptr, &type, (BYTE*)&data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type == REG_DWORD && data != 0) { Log("XR add-on %s (%s): switched off in Windows", manifest, rootName); continue; }
        std::string text = ReadText(manifest);
        std::string name = JsonString(text, "name"), lib = JsonString(text, "library_path"), off = JsonString(text, "disable_environment");
        Log("XR add-on %s (%s): %s, off switch %s", name.c_str(), rootName, lib.c_str(), off.empty() ? "none" : off.c_str());
        if (!off.empty()) { g_offSwitches.push_back(off); g_offNames += (g_offNames.empty() ? "" : ", ") + name; }
        else g_stuckNames += (g_stuckNames.empty() ? "" : ", ") + name;
    }
    RegCloseKey(key);
}

static bool StartXr() {
    g_offSwitches.clear(); g_offNames.clear(); g_stuckNames.clear();
    ListLayers(HKEY_LOCAL_MACHINE, "HKLM"); ListLayers(HKEY_CURRENT_USER, "HKCU");
    char rt[1024]; DWORD rs = sizeof rt;
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof rt) > 0) Log("XR runtime manifest (XR_RUNTIME_JSON) %s", rt);
    else if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, rt, &rs) == ERROR_SUCCESS) Log("XR runtime manifest %s", rt);
    else Log("XR no active OpenXR runtime set in Windows");

    char path[MAX_PATH]; HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&StartXr, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    *(strrchr(path, '\\') + 1) = 0; strcat_s(path, "openxr_loader.dll");
    g_loader = LoadLibraryA(path);
    if (!g_loader) { Log("XR could not load %s, error %lu", path, GetLastError()); return false; }
    g_gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!g_gipa) return false;

    PFN_xrCreateInstance createInstance; if (!GetFn("xrCreateInstance", createInstance)) return false;
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "AI Limit VR");  // the name SteamVR shows (Steve, 2026-11-20)
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "Unity 2022.3 HDRP");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    XrResult created = createInstance(&ici, &g_inst);
    if (created == XR_ERROR_FILE_ACCESS_ERROR && !g_offSwitches.empty()) {
        // -32: an add-on (API layer) refused to load. Try once more with every add-on that has an off switch switched
        // off for this game only (TECHNIQUES "OpenXR start failures" item 4).
        Log("XR could not load an add-on (-32). Trying again with these switched off for this game only: %s", g_offNames.c_str());
        for (auto& v : g_offSwitches) SetEnvironmentVariableA(v.c_str(), "1");
        created = createInstance(&ici, &g_inst);
        Log(XR_SUCCEEDED(created) ? "XR started with those add-ons switched off" : "XR still failing with those add-ons off: %d", (int)created);
    }
    XRCHECK(created);
    PFN_xrGetInstanceProperties props;
    if (GetFn("xrGetInstanceProperties", props)) {
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(props(g_inst, &ip))) { strcpy_s(g_runtime, ip.runtimeName); Log("XR runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion)); }
    }
    PFN_xrGetSystem getSystem; if (!GetFn("xrGetSystem", getSystem)) return false;
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(getSystem(g_inst, &sgi, &g_sys));
    PFN_xrGetSystemProperties sysProps;
    if (GetFn("xrGetSystemProperties", sysProps)) {
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(sysProps(g_inst, g_sys, &sp))) { strcpy_s(g_headset, sp.systemName); Log("XR headset %s, vendor %u", sp.systemName, sp.vendorId); }
    }
    PFN_xrEnumerateViewConfigurationViews enumViews;
    if (GetFn("xrEnumerateViewConfigurationViews", enumViews)) {
        XrViewConfigurationView v[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t vc = 0;
        if (XR_SUCCEEDED(enumViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, v))) {
            g_recW = (int)v[0].recommendedImageRectWidth; g_recH = (int)v[0].recommendedImageRectHeight;
            Log("XR headset recommends %dx%d per eye (largest %ux%u)", (int)g_recW, (int)g_recH, v[0].maxImageRectWidth, v[0].maxImageRectHeight);
        }
    }
    if (g_recW <= 0) { Log("XR the headset gave no eye size"); return false; }
    g_eyeW = (((int)lroundf(g_recW * g_resolution)) + 1) & ~1; g_eyeH = (((int)lroundf(g_recH * g_resolution)) + 1) & ~1;
    if (g_eyeW > 8192) g_eyeW = 8192; if (g_eyeH > 8192) g_eyeH = 8192;
    Log("RENDERSIZE eye image %dx%d = recommended %dx%d x resolution %.2f", g_eyeW, g_eyeH, (int)g_recW, (int)g_recH, g_resolution);
    PFN_xrGetD3D11GraphicsRequirementsKHR req11; if (!GetFn("xrGetD3D11GraphicsRequirementsKHR", req11)) return false;
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(req11(g_inst, g_sys, &req));
    if (!CreateDeviceForRuntime(req.adapterLuid)) return false;
    PFN_xrCreateSession createSession; if (!GetFn("xrCreateSession", createSession)) return false;
    XrGraphicsBindingD3D11KHR bind{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; bind.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = &bind; sci.systemId = g_sys;
    XRCHECK(createSession(g_inst, &sci, &g_sess));
    if (!GetFn("xrCreateReferenceSpace", p_createSpace)) return false;
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rsci.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(p_createSpace(g_sess, &rsci, &g_space));
    PFN_xrEnumerateSwapchainFormats enumFormats; if (!GetFn("xrEnumerateSwapchainFormats", enumFormats)) return false;
    uint32_t n = 0; XRCHECK(enumFormats(g_sess, 0, &n, nullptr));
    std::vector<int64_t> formats(n); XRCHECK(enumFormats(g_sess, n, &n, formats.data()));
    for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) g_format = f;
    if (!g_format) for (auto f : formats) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) g_format = f;
    if (!g_format) { Log("XR the runtime offers no RGBA8 swapchain format"); return false; }
    if (!GetFn("xrCreateSwapchain", p_createSwap) || !GetFn("xrEnumerateSwapchainImages", p_enumImages) || !GetFn("xrDestroySwapchain", p_destroySwap) ||
        !GetFn("xrPollEvent", p_poll) || !GetFn("xrBeginSession", p_beginSession) || !GetFn("xrEndSession", p_endSession) ||
        !GetFn("xrWaitFrame", p_waitFrame) || !GetFn("xrBeginFrame", p_beginFrame) || !GetFn("xrEndFrame", p_endFrame) ||
        !GetFn("xrLocateViews", p_locate) || !GetFn("xrLocateSpace", p_locateSpace) || !GetFn("xrAcquireSwapchainImage", p_acquire) ||
        !GetFn("xrWaitSwapchainImage", p_waitImage) || !GetFn("xrReleaseSwapchainImage", p_release) ||
        !GetFn("xrDestroySession", p_destroySession) || !GetFn("xrDestroyInstance", p_destroyInstance)) return false;
    if (!CreateRectPass()) return false;
    for (int e = 0; e < 2; e++) if (!MakeSwap(g_proj[e], g_eyeW, g_eyeH, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (!MakeSwap(g_hudSwap, g_hudW, g_hudH, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) return false;
    bool vd = strstr(g_runtime, "VirtualDesktop") || strstr(g_runtime, "Virtual Desktop");
    bool steady = g_picture == 1 || (g_picture == 0 && vd);
    g_layerInUse = steady ? 1 : 0;
    Log("XR PICTURE setting %s, runtime \"%s\", headset \"%s\", flat panels %s, swapchain format %d, eye images %dx%d",
        g_picture == 1 ? "Steady" : g_picture == 2 ? "Panel" : "Auto", g_runtime, g_headset, steady ? "drawn into the eyes (Steady, projection)" : "quad layers (Panel)",
        (int)g_format, g_eyeW, g_eyeH);
    return CreateSlots();
}

// ------------------------------------------------------------------------------------------------ maths
struct V3 { float x, y, z; };
static V3 Rotate(const float q[4], V3 v) {
    V3 t{2 * (q[1] * v.z - q[2] * v.y), 2 * (q[2] * v.x - q[0] * v.z), 2 * (q[0] * v.y - q[1] * v.x)};
    return {v.x + q[3] * t.x + (q[1] * t.z - q[2] * t.y), v.y + q[3] * t.y + (q[2] * t.x - q[0] * t.z), v.z + q[3] * t.z + (q[0] * t.y - q[1] * t.x)};
}
static void QMul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static float g_viewPoint[3] = {0, 0, 0}, g_viewTurn[4] = {0, 0, 0, 1};  // recenter point and yaw (xyzw)
static uint32_t g_recenters = 0;

static void PlaceView(const XrView* v) {
    float p[3] = {(v[0].pose.position.x + v[1].pose.position.x) * 0.5f, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f, (v[0].pose.position.z + v[1].pose.position.z) * 0.5f};
    const XrQuaternionf& q = v[0].pose.orientation;
    float qq[4] = {q.x, q.y, q.z, q.w};
    V3 fwd = Rotate(qq, {0, 0, -1});
    float yaw = atan2f(-fwd.x, -fwd.z);
    g_viewTurn[0] = 0; g_viewTurn[1] = sinf(yaw * 0.5f); g_viewTurn[2] = 0; g_viewTurn[3] = cosf(yaw * 0.5f);
    memcpy(g_viewPoint, p, sizeof p);
    g_recenters++;
    Log("RECENTER %u view (%.3f, %.3f, %.3f) facing %.1f deg", g_recenters, p[0], p[1], p[2], yaw * 57.2958f);
}

// A room rectangle's corners in the eye's clip space: room -> eye -> lens (Ori VR's StageCorners).
static void Corners(const float* rect, const float* size, const XrPosef& eye, const XrFovf& fov, float out[16]) {
    float inv[4] = {-eye.orientation.x, -eye.orientation.y, -eye.orientation.z, eye.orientation.w};
    float hw = size[0] * 0.5f, hh = size[1] * 0.5f;
    const V3 local[4] = {{-hw, hh, 0}, {hw, hh, 0}, {-hw, -hh, 0}, {hw, -hh, 0}};
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight), u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    const float n = 0.05f, f = 100.0f;
    for (int i = 0; i < 4; i++) {
        V3 w = Rotate(rect + 3, local[i]);
        V3 v = Rotate(inv, {rect[0] + w.x - eye.position.x, rect[1] + w.y - eye.position.y, rect[2] + w.z - eye.position.z});
        out[i * 4 + 0] = (2 * v.x + (l + r) * v.z) / (r - l);
        out[i * 4 + 1] = (2 * v.y + (u + d) * v.z) / (u - d);
        out[i * 4 + 2] = (f / (f - n)) * -v.z - f * n / (f - n);
        out[i * 4 + 3] = -v.z;
    }
}

// ------------------------------------------------------------------------------------------------ game side (render thread)
struct Pending { void* tex[3]; Views views; uint32_t frame; };
static std::mutex g_pendMx;
static std::deque<Pending> g_pending;
void QueueFrame(void* texL, void* texR, void* hud, const Views& used, uint32_t frameNo) {
    std::lock_guard<std::mutex> lk(g_pendMx);
    if (g_pending.size() > 4) g_pending.pop_front();
    g_pending.push_back({{texL, texR, hud}, used, frameNo});
    std::lock_guard<std::mutex> sl(g_statMx); g_stats.queued++;
}
static ID3D11Device* g_gameDev; static ID3D11DeviceContext* g_gameCtx;
// GPU time of the frame's rendering: a timestamp at the frame's first render call (event 3) and one after the eye copy
// (event 1), read back a few frames later. Never waits on the GPU.
struct GpuQ { ID3D11Query *dis = nullptr, *t0 = nullptr, *t1 = nullptr; bool open = false, ended = false; };
static GpuQ g_gq[6]; static int g_gqi = 0;
static void GpuBegin() {
    if (!g_gameDev) return;
    GpuQ& q = g_gq[g_gqi % 6];
    if (!q.dis) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}; g_gameDev->CreateQuery(&d, &q.dis);
        d.Query = D3D11_QUERY_TIMESTAMP; g_gameDev->CreateQuery(&d, &q.t0); g_gameDev->CreateQuery(&d, &q.t1);
    }
    if (q.open || q.ended) return;  // still waiting for an older result: skip this frame
    g_gameCtx->Begin(q.dis); g_gameCtx->End(q.t0); q.open = true;
}
static void GpuEnd() {
    if (!g_gameDev) return;
    GpuQ& q = g_gq[g_gqi % 6];
    if (q.open) { g_gameCtx->End(q.t1); g_gameCtx->End(q.dis); q.open = false; q.ended = true; g_gqi++; }
    for (auto& r : g_gq) {
        if (!r.ended) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
        if (g_gameCtx->GetData(r.dis, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (g_gameCtx->GetData(r.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (g_gameCtx->GetData(r.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        r.ended = false;
        if (dj.Disjoint || b < a) continue;
        double ms = (b - a) * 1000.0 / dj.Frequency;
        std::lock_guard<std::mutex> sl(g_statMx); g_stats.gpuSamples++; g_gpuSum += ms; if (ms > g_stats.gpuMsMax) g_stats.gpuMsMax = ms;
    }
}
static void __stdcall OnRenderEvent(int id) {
    if (id == 3) { GpuBegin(); return; }
    Pending p;
    { std::lock_guard<std::mutex> lk(g_pendMx); if (g_pending.empty()) return; p = g_pending.front(); g_pending.pop_front(); }
    if (!g_slotsReady || !p.tex[0] || !p.tex[1]) return;
    double t0 = Ms();
    if (!g_gameDev) {
        ((ID3D11Texture2D*)p.tex[0])->GetDevice(&g_gameDev); g_gameDev->GetImmediateContext(&g_gameCtx);
        for (auto& s : g_slots) for (int e = 0; e < kTex; e++) {
            if (FAILED(g_gameDev->OpenSharedResource(s.share[e], __uuidof(ID3D11Texture2D), (void**)&s.gameTex[e]))) { Log("XR game side could not open a shared slot"); return; }
            s.gameTex[e]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.gameMx[e]);
        }
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)p.tex[0])->GetDesc(&d);
        Log("XR game side opened the shared ring on the game's device; game eye texture %ux%u format %d", d.Width, d.Height, (int)d.Format);
        if (p.tex[2]) { ((ID3D11Texture2D*)p.tex[2])->GetDesc(&d); Log("XR game HUD texture %ux%u format %d", d.Width, d.Height, (int)d.Format); }
    }
    // The oldest-written slot the XR side is not holding. Never wait: a busy slot is skipped and counted.
    Slot* best = nullptr;
    for (auto& s : g_slots) if (!s.xrHolds && (!best || s.seq < best->seq)) best = &s;  // seq 0 = free, else the oldest unshown
    if (!best) { std::lock_guard<std::mutex> sl(g_statMx); g_stats.skippedNoSlots++; GpuEnd(); return; }
    int got = 0;
    for (; got < kTex; got++) if (best->gameMx[got]->AcquireSync(0, 0) != S_OK) break;
    if (got < kTex) { for (int i = 0; i < got; i++) best->gameMx[i]->ReleaseSync(0); std::lock_guard<std::mutex> sl(g_statMx); g_stats.skippedBusy++; GpuEnd(); return; }
    for (int e = 0; e < 2; e++) g_gameCtx->CopyResource(best->gameTex[e], (ID3D11Texture2D*)p.tex[e]);
    best->hasHud = p.tex[2] != nullptr;
    if (p.tex[2]) g_gameCtx->CopyResource(best->gameTex[2], (ID3D11Texture2D*)p.tex[2]);
    best->views = p.views;
    best->seq = ++g_seq;
    for (int e = 0; e < kTex; e++) best->gameMx[e]->ReleaseSync(1);
    GpuEnd();
    double ms = Ms() - t0;
    std::lock_guard<std::mutex> sl(g_statMx); g_stats.copied++; if (ms > g_stats.copyMsMax) g_stats.copyMsMax = ms;
}
void* RenderEventFunc() { return (void*)&OnRenderEvent; }

// ------------------------------------------------------------------------------------------------ XR thread
static Slot* g_shown = nullptr;
// Written slots the XR side has not taken (key 1). The newest is shown; older ones are handed straight back (key 0),
// so the game side always finds a free slot unless the XR side is mid-draw.
static uint32_t g_dropped = 0;
static bool AcquireAll(Slot& s) {
    int got = 0;
    for (; got < kTex; got++) if (s.xrMx[got]->AcquireSync(1, 0) != S_OK) break;
    if (got == kTex) return true;
    for (int i = 0; i < got; i++) s.xrMx[i]->ReleaseSync(1);
    return false;
}
static void ReleaseAll(Slot& s, UINT key) { for (int i = 0; i < kTex; i++) s.xrMx[i]->ReleaseSync(key); }
static Slot* TakeNewest() {
    Slot* newest = nullptr;
    for (auto& s : g_slots) if (s.seq && !s.xrHolds && (!g_shown || s.seq > g_shown->seq) && (!newest || s.seq > newest->seq)) newest = &s;
    for (auto& s : g_slots) {
        if (&s == newest || &s == g_shown || !s.seq || s.xrHolds) continue;
        if (AcquireAll(s)) { ReleaseAll(s, 0); s.seq = 0; g_dropped++; }
    }
    if (!newest || !AcquireAll(*newest)) return nullptr;
    newest->xrHolds = true;
    if (g_shown) { ReleaseAll(*g_shown, 0); g_shown->xrHolds = false; }
    g_shown = newest;
    return newest;
}

static ID3D11ShaderResourceView* g_menuSrv;  // this frame's menu, null when hidden
static void DrawRect(ID3D11ShaderResourceView* srv, const float* corners, bool flip = false) {
    float cb[20]; memcpy(cb, corners, 16 * sizeof(float)); cb[16] = flip ? 1.f : 0.f; cb[17] = cb[18] = cb[19] = 0;
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(g_vs, nullptr, 0); g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0); g_ctx->PSSetSamplers(0, 1, &g_linear); g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(4, 0);
    ID3D11ShaderResourceView* no = nullptr; g_ctx->PSSetShaderResources(0, 1, &no);
}

// Panels in the room (LOCAL space): pose xyz + xyzw, size w h.
static float g_hudPose[7] = {0, 0, -1.6f, 0, 0, 0, 1}, g_hudSize[2] = {1.6f, 0.9f};
static float g_menuPose[7] = {0, 0, -0.9f, 0, 0, 0, 1}, g_menuSize[2] = {0.9f, 0.5625f};
static bool g_hudShow = false;

static void PlaceAt(float out[7], const float base[3], const float turn[4], float x, float y, float dist) {
    V3 at = Rotate(turn, {x, y, -dist});
    out[0] = base[0] + at.x; out[1] = base[1] + at.y; out[2] = base[2] + at.z;
    memcpy(out + 3, turn, 4 * sizeof(float));
}

// Lab: the next composed left-eye projection image (game picture plus panels when drawn inside) to a raw file.
static std::atomic<bool> g_wantXrSnap{false};
static std::string g_xrSnapPath;
void SnapComposed(const char* path) { g_xrSnapPath = path; g_wantXrSnap = true; }
static void WriteComposed(ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) { Log("XRSNAP staging failed"); return; }
    g_ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) {
        FILE* f = nullptr; fopen_s(&f, g_xrSnapPath.c_str(), "wb");
        if (f) { uint32_t hdr[4] = {d.Width, d.Height, (uint32_t)d.Format, 4}; fwrite(hdr, 4, 4, f);
            for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t*)map.pData + y * map.RowPitch, 1, d.Width * 4, f); fclose(f); }
        g_ctx->Unmap(st, 0);
        Log("XRSNAP composed left eye %ux%u format %d written to %s (top row first)", d.Width, d.Height, (int)d.Format, g_xrSnapPath.c_str());
    }
    st->Release();
}

// Projection image for one eye: the game's eye picture full frame, plus the panels on top when they go into the eyes.
static bool DrawEye(int e, XrCompositionLayerProjectionView& out, bool panelsInside) {
    Swap& s = g_proj[e];
    uint32_t idx = 0;
    if (XR_FAILED(p_acquire(s.handle, nullptr, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 100000000;
    if (XR_FAILED(p_waitImage(s.handle, &wi))) { p_release(s.handle, nullptr); return false; }
    if (!s.rtv[idx]) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = (DXGI_FORMAT)g_format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        g_dev->CreateRenderTargetView(s.images[idx].texture, &rv, &s.rtv[idx]);
    }
    D3D11_VIEWPORT vp{0, 0, (float)s.w, (float)s.h, 0, 1};
    g_ctx->RSSetViewports(1, &vp); g_ctx->RSSetState(g_noCull); g_ctx->IASetInputLayout(nullptr);
    g_ctx->OMSetRenderTargets(1, &s.rtv[idx], nullptr);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    static const float full[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};
    DrawRect(g_shown->srv[e], full, true);
    const Views& v = g_shown->views;
    XrPosef pose; pose.position = {v.pose[e][0], v.pose[e][1], v.pose[e][2]}; pose.orientation = {v.pose[e][3], v.pose[e][4], v.pose[e][5], v.pose[e][6]};
    XrFovf fov{v.fov[e][0], v.fov[e][1], v.fov[e][2], v.fov[e][3]};
    if (panelsInside) {
        float corners[16];
        g_ctx->OMSetBlendState(g_alphaBlend, nullptr, 0xffffffff);
        if (g_hudShow && g_shown->hasHud) { Corners(g_hudPose, g_hudSize, pose, fov, corners); DrawRect(g_shown->srv[2], corners, true); }
        if (g_menuSrv) { Corners(g_menuPose, g_menuSize, pose, fov, corners); DrawRect(g_menuSrv, corners); }
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    { ID3D11RenderTargetView* none = nullptr; g_ctx->OMSetRenderTargets(1, &none, nullptr); }
    if (e == 0 && g_wantXrSnap) { g_wantXrSnap = false; WriteComposed(s.images[idx].texture); }
    p_release(s.handle, nullptr);
    out = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    out.pose = pose; out.fov = fov;
    out.subImage.swapchain = s.handle; out.subImage.imageRect.extent = {s.w, s.h};
    return true;
}

static bool CopyQuad(Swap& s, ID3D11Texture2D* src, int w, int h, const float pose[7], const float size[2], XrCompositionLayerQuad& q) {
    uint32_t idx = 0;
    if (XR_FAILED(p_acquire(s.handle, nullptr, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 100000000;
    if (XR_FAILED(p_waitImage(s.handle, &wi))) { p_release(s.handle, nullptr); return false; }
    g_ctx->CopyResource(s.images[idx].texture, src);
    p_release(s.handle, nullptr);
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    q.space = g_space; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = s.handle; q.subImage.imageRect.extent = {w, h};
    q.pose.position = {pose[0], pose[1], pose[2]};
    q.pose.orientation = {pose[3], pose[4], pose[5], pose[6]};
    q.size = {size[0], size[1]};
    return true;
}

// The HUD quad: drawn upright into its swapchain (the game's texture is stored bottom row first).
static bool DrawQuad(Swap& s, ID3D11ShaderResourceView* srv, const float pose[7], const float size[2], XrCompositionLayerQuad& q) {
    uint32_t idx = 0;
    if (XR_FAILED(p_acquire(s.handle, nullptr, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 100000000;
    if (XR_FAILED(p_waitImage(s.handle, &wi))) { p_release(s.handle, nullptr); return false; }
    if (!s.rtv[idx]) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = (DXGI_FORMAT)g_format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        g_dev->CreateRenderTargetView(s.images[idx].texture, &rv, &s.rtv[idx]);
    }
    float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(s.rtv[idx], clear);
    D3D11_VIEWPORT vp{0, 0, (float)s.w, (float)s.h, 0, 1};
    g_ctx->RSSetViewports(1, &vp); g_ctx->RSSetState(g_noCull); g_ctx->IASetInputLayout(nullptr);
    g_ctx->OMSetRenderTargets(1, &s.rtv[idx], nullptr); g_ctx->OMSetDepthStencilState(nullptr, 0); g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    static const float full[16] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1, 1, -1, 0, 1};
    DrawRect(srv, full, true);
    { ID3D11RenderTargetView* none = nullptr; g_ctx->OMSetRenderTargets(1, &none, nullptr); }
    p_release(s.handle, nullptr);
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    q.space = g_space; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = s.handle; q.subImage.imageRect.extent = {s.w, s.h};
    q.pose.position = {pose[0], pose[1], pose[2]};
    q.pose.orientation = {pose[3], pose[4], pose[5], pose[6]};
    q.size = {size[0], size[1]};
    return true;
}

static void XrThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    QueryPerformanceFrequency(&g_qpf);
    g_state = Starting;
    if (!StartXr()) { g_state = Failed; Log("XR VR DID NOT START, error %d. The game runs flat.", (int)g_lastError); return; }
    bool running = false;
    XrSessionState sstate = XR_SESSION_STATE_UNKNOWN;
    double focusedAt = -1;
    while (!g_quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (p_poll(g_inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                sstate = ((XrEventDataSessionStateChanged&)ev).state;
                Log("XR session state %d", (int)sstate);
                if (sstate == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = p_beginSession(g_sess, &bi); running = XR_SUCCEEDED(r); if (running) g_state = Running;
                    Log("XR session begun: %d", (int)r);
                } else if (sstate == XR_SESSION_STATE_STOPPING) { p_endSession(g_sess); running = false; g_state = Stopped; }
                else if (sstate == XR_SESSION_STATE_EXITING || sstate == XR_SESSION_STATE_LOSS_PENDING) { running = false; g_state = Stopped; g_quit = true; }
                else if (sstate == XR_SESSION_STATE_FOCUSED && focusedAt < 0) { focusedAt = Ms(); }
            } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                Log("XR the runtime recentred (reference space change): recentring the view"); g_wantRecenter = true;
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { g_quit = true; }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { Sleep(10); continue; }
        double t0 = Ms();
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(p_waitFrame(g_sess, nullptr, &fs))) { Sleep(1); continue; }
        double t1 = Ms();
        p_beginFrame(g_sess, nullptr);
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        // Poses for the game side: predicted one display period further, since the game renders a frame ahead of display.
        li.displayTime = fs.predictedDisplayTime; li.space = g_space;
        uint32_t vc = 0;
        bool valid = XR_SUCCEEDED(p_locate(g_sess, &li, &vs, 2, &vc, views)) &&
                     (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        // Automatic recenter once the session is FOCUSED (Virtual Desktop reports a stand-in head pose before that).
        // A session that never gets focus still recenters after 20 s.
        bool focused = sstate == XR_SESSION_STATE_FOCUSED;
        static double startedAt = Ms();
        if (valid && g_wantRecenter && ((focused && Ms() - focusedAt > 500) || Ms() - startedAt > 20000)) { g_wantRecenter = false; PlaceView(views); }
        float headPos[3] = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f, (views[0].pose.position.y + views[1].pose.position.y) * 0.5f, (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
        float headRot[4] = {views[0].pose.orientation.x, views[0].pose.orientation.y, views[0].pose.orientation.z, views[0].pose.orientation.w};
        {
            std::lock_guard<std::mutex> lk(g_viewsMx);
            Views& o = g_views;
            o.valid = valid; o.focused = focused; o.sessionState = (int)sstate; o.periodSec = fs.predictedDisplayPeriod / 1e9;
            o.displayTime = fs.predictedDisplayTime; o.recenters = g_recenters;
            float inv[4] = {-g_viewTurn[0], -g_viewTurn[1], -g_viewTurn[2], g_viewTurn[3]};
            for (int e = 0; e < 2; e++) {
                const XrPosef& p = views[e].pose;
                float pp[7] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
                memcpy(o.pose[e], pp, sizeof pp);
                V3 l = Rotate(inv, {p.position.x - g_viewPoint[0], p.position.y - g_viewPoint[1], p.position.z - g_viewPoint[2]});
                o.local[e][0] = l.x; o.local[e][1] = l.y; o.local[e][2] = l.z;
                QMul(inv, pp + 3, o.localRot[e]);
                o.fov[e][0] = views[e].fov.angleLeft; o.fov[e][1] = views[e].fov.angleRight; o.fov[e][2] = views[e].fov.angleUp; o.fov[e][3] = views[e].fov.angleDown;
            }
        }
        bool fresh = TakeNewest() != nullptr;
        // Panels: the HUD (Fixed in the room at recenter, or held in front of the head), the menu (in the room).
        Panel hud; { std::lock_guard<std::mutex> lk(g_hudMx); hud = g_hud; }
        g_hudShow = hud.mode != 2;
        g_hudSize[0] = hud.width; g_hudSize[1] = hud.width * g_hudH / (float)g_hudW;
        if (hud.mode == 1 && valid) PlaceAt(g_hudPose, headPos, headRot, hud.x, hud.y, hud.distance);
        else PlaceAt(g_hudPose, g_viewPoint, g_viewTurn, hud.x, hud.y, hud.distance);
        MenuSnapshot place;
        g_menuSrv = g_recenters ? DrawMenu(g_dev, g_ctx, g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Log, &place) : nullptr;
        if (g_menuSrv) { PlaceAt(g_menuPose, g_viewPoint, g_viewTurn, 0, place.height, place.distance); g_menuSize[0] = 0.9f * place.size; g_menuSize[1] = g_menuSize[0] * MenuAspect(); }
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime; fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2];
        XrCompositionLayerQuad quads[2];
        const XrCompositionLayerBaseHeader* layers[3]; uint32_t nl = 0;
        // Only pictures rendered from a real head pose: the first frames after VR starts can carry no pose (zero
        // quaternion, zero field of view), which SteamVR refuses but Virtual Desktop may crash on.
        if (fs.shouldRender && g_shown && g_shown->views.valid && g_shown->views.pose[0][6] * g_shown->views.pose[0][6] + g_shown->views.pose[0][3] * g_shown->views.pose[0][3] + g_shown->views.pose[0][4] * g_shown->views.pose[0][4] + g_shown->views.pose[0][5] * g_shown->views.pose[0][5] > 0.5f) {
            bool inside = g_layerInUse == 1;
            if (DrawEye(0, pv[0], inside) && DrawEye(1, pv[1], inside)) {
                proj.space = g_space; proj.viewCount = 2; proj.views = pv; layers[nl++] = (XrCompositionLayerBaseHeader*)&proj;
                if (!inside) {
                    if (g_hudShow && g_shown->hasHud && DrawQuad(g_hudSwap, g_shown->srv[2], g_hudPose, g_hudSize, quads[0])) layers[nl++] = (XrCompositionLayerBaseHeader*)&quads[0];
                    if (g_menuSrv) {
                        if (!g_menuSwap.handle) MakeSwap(g_menuSwap, 1280, 800, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT);
                        if (g_menuSwap.handle && CopyQuad(g_menuSwap, MenuTexture(), 1280, 800, g_menuPose, g_menuSize, quads[1])) layers[nl++] = (XrCompositionLayerBaseHeader*)&quads[1];
                    }
                }
            }
        }
        fe.layerCount = nl; fe.layers = layers;
        double t2 = Ms();
        p_endFrame(g_sess, &fe);
        double t3 = Ms();
        std::lock_guard<std::mutex> sl(g_statMx);
        g_stats.xrFrames++; if (!fresh) g_stats.xrNoNewImage++; if (nl) g_stats.layersSent++;
        double w = t1 - t0, en = t3 - t2;
        g_waitSum += w; g_endSum += en; g_timed++;
        if (w > g_stats.waitMsMax) g_stats.waitMsMax = w; if (en > g_stats.endMsMax) g_stats.endMsMax = en;
    }
    if (g_sess) { p_endSession(g_sess); }
    g_state = Stopped;
    Log("XR thread ended");
}

void Start() {
    int expected = Off;
    if (!g_state.compare_exchange_strong(expected, Starting)) return;
    g_thread = std::thread(XrThread);
    g_thread.detach();
}
void Shutdown() { g_quit = true; }

}  // namespace aixr
