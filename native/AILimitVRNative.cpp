// AI Limit VR, native mod (build 1, Phase 1). Route B: loaded by the winhttp.dll proxy, no BepInEx.
// Ported from Ori WotW VR's native mod (same author, MIT): IL2CPP lookups by name through the game's exported IL2CPP API,
// setup on the main thread inside the first il2cpp_runtime_invoke, MinHook hooks, the frame cap, logging, the menu.
// AI Limit renders with HDRP. Two eye cameras (each its own HDRP history: TAA, exposure) render inside the same
// HDRenderPipeline.Render call, in the same game tick, at the game camera's pose plus each eye's head pose. The game's
// own camera is left out of that call while VR runs (saves a full render). The game's overlay canvases (HUD and menus)
// render through our own UI camera into a texture shown as a panel. OpenXR runs on its own thread (xr\AIXR.cpp).
// Feasibility findings this rests on: RE_NOTES.md "Feasibility evening". MinHook (TsudaKageyu, BSD-2) is compiled in.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <fstream>
#include <sstream>
#include "MinHook.h"
#include "../xr/AIXR.h"

#define MOD_VERSION "0.4.0"
#define MOD_BUILD 4

// ---------------------------------------------------------------- log
static std::wstring g_gameDir;
static std::mutex g_logMx;
static FILE* g_log = nullptr;
static LARGE_INTEGER g_qpf, g_qp0;
static double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - g_qp0.QuadPart) / g_qpf.QuadPart; }
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- settings (AILimitVR\AILimitVR.ini)
struct Settings {
    float resolution = 0.95f;   // defaults: Steve's RigA settings at the gate (2026-11-20)
    int picture = 0;            // 0 Auto, 1 Steady, 2 Panel
    int hudMode = 0;            // 0 Fixed, 1 Follows head, 2 Hidden
    float hudWidth = 1.85f, hudX = 0.0f, hudY = -0.17f, hudDistance = 1.25f;
    float menuSize = 1.0f, menuHeight = 0.0f, menuDistance = 0.9f;
    int blockNetwork = 1;       // the game's internet connections are refused and logged
    float worldScale = 1.0f;    // game metres per real metre of head movement and eye gap (1 life size, higher: smaller world, more depth)
    int upscaler = 3;           // 0 TAA, 1 DLAA, 2 DLSS Quality, 3 DLSS Balanced, 4 DLSS Performance, 5 DLSS Ultra Performance
    float lockOnLift = 0;       // lock-on marker raised by this many screen pixels
    int decoupledPitch = 1;     // 1: the game camera's up/down tilt and roll do not tilt the view; the head looks up and down (Steve, 2026-11-20)
};
static Settings g_set;
static std::wstring IniPath() { return g_gameDir + L"AILimitVR\\AILimitVR.ini"; }
static float IniF(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t b[64], d[64]; swprintf_s(d, L"%g", def);
    GetPrivateProfileStringW(sec, key, d, b, 64, IniPath().c_str()); return (float)_wtof(b);
}
static void IniW(const wchar_t* sec, const wchar_t* key, float v) {
    wchar_t b[64]; swprintf_s(b, L"%g", v); WritePrivateProfileStringW(sec, key, b, IniPath().c_str());
}
static void LoadSettings() {
    g_set.resolution = IniF(L"VR", L"Resolution", 0.95f);
    g_set.worldScale = IniF(L"VR", L"WorldScale", 1.0f); g_set.upscaler = (int)IniF(L"VR", L"Upscaler", 3); g_set.lockOnLift = IniF(L"HUD", L"LockOnLift", 0); g_set.decoupledPitch = (int)IniF(L"VR", L"DecoupledPitch", 1);
    g_set.picture = (int)IniF(L"VR", L"Picture", 0);
    g_set.hudMode = (int)IniF(L"HUD", L"Mode", 0);
    g_set.hudWidth = IniF(L"HUD", L"Size", 1.85f); g_set.hudX = IniF(L"HUD", L"X", 0.0f);
    g_set.hudY = IniF(L"HUD", L"Height", -0.17f); g_set.hudDistance = IniF(L"HUD", L"Distance", 1.25f);
    g_set.menuSize = IniF(L"Menu", L"Size", 1.0f); g_set.menuHeight = IniF(L"Menu", L"Height", 0.0f); g_set.menuDistance = IniF(L"Menu", L"Distance", 0.9f);
    g_set.blockNetwork = (int)IniF(L"Network", L"Block", 1);
}
static void SaveSettings() {
    IniW(L"VR", L"Resolution", g_set.resolution); IniW(L"VR", L"Picture", (float)g_set.picture);
    IniW(L"VR", L"WorldScale", g_set.worldScale); IniW(L"VR", L"Upscaler", (float)g_set.upscaler); IniW(L"HUD", L"LockOnLift", g_set.lockOnLift); IniW(L"VR", L"DecoupledPitch", (float)g_set.decoupledPitch);
    IniW(L"HUD", L"Mode", (float)g_set.hudMode); IniW(L"HUD", L"Size", g_set.hudWidth); IniW(L"HUD", L"X", g_set.hudX);
    IniW(L"HUD", L"Height", g_set.hudY); IniW(L"HUD", L"Distance", g_set.hudDistance);
    IniW(L"Menu", L"Size", g_set.menuSize); IniW(L"Menu", L"Height", g_set.menuHeight); IniW(L"Menu", L"Distance", g_set.menuDistance);
    IniW(L"Network", L"Block", (float)g_set.blockNetwork);
}

// ---------------------------------------------------------------- il2cpp
struct Il2CppObject { void* klass; void* monitor; };
struct Il2CppArray { void* klass; void* monitor; void* bounds; uintptr_t max_length; void* data[1]; };
struct MethodInfo { void* methodPointer; };
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Color { float r, g, b, a; };
struct Mat4 { float m[16]; };  // Unity column-major: element (row r, col c) at m[c*4+r]
static float& M(Mat4& a, int r, int c) { return a.m[c * 4 + r]; }

#define IL2CPP_FN(ret, name, args) typedef ret(*name##_t) args; static name##_t name;
IL2CPP_FN(void*, il2cpp_domain_get, ())
IL2CPP_FN(void**, il2cpp_domain_get_assemblies, (void*, size_t*))
IL2CPP_FN(void*, il2cpp_assembly_get_image, (void*))
IL2CPP_FN(void*, il2cpp_class_from_name, (void*, const char*, const char*))
IL2CPP_FN(MethodInfo*, il2cpp_class_get_methods, (void*, void**))
IL2CPP_FN(const char*, il2cpp_method_get_name, (MethodInfo*))
IL2CPP_FN(uint32_t, il2cpp_method_get_param_count, (MethodInfo*))
IL2CPP_FN(void*, il2cpp_method_get_param, (MethodInfo*, uint32_t))
IL2CPP_FN(char*, il2cpp_type_get_name, (void*))
IL2CPP_FN(void*, il2cpp_resolve_icall, (const char*))
IL2CPP_FN(Il2CppObject*, il2cpp_object_new, (void*))
IL2CPP_FN(void*, il2cpp_class_get_field_from_name, (void*, const char*))
IL2CPP_FN(size_t, il2cpp_field_get_offset, (void*))
IL2CPP_FN(void, il2cpp_field_static_get_value, (void*, void*))
IL2CPP_FN(uint32_t, il2cpp_gchandle_new, (Il2CppObject*, bool))
IL2CPP_FN(void*, il2cpp_class_get_parent, (void*))
IL2CPP_FN(void, il2cpp_free, (void*))
IL2CPP_FN(void*, il2cpp_class_get_type, (void*))
IL2CPP_FN(Il2CppObject*, il2cpp_type_get_object, (void*))
IL2CPP_FN(void*, il2cpp_object_get_class, (Il2CppObject*))
IL2CPP_FN(const char*, il2cpp_class_get_name, (void*))
IL2CPP_FN(Il2CppObject*, il2cpp_string_new, (const char*))

static HMODULE g_ga;
static bool BindIl2cpp() {
#define B(n) n = (n##_t)GetProcAddress(g_ga, #n); if (!n) { Log("IL2CPP export missing: " #n); return false; }
    B(il2cpp_domain_get) B(il2cpp_domain_get_assemblies) B(il2cpp_assembly_get_image) B(il2cpp_class_from_name)
    B(il2cpp_class_get_methods) B(il2cpp_method_get_name) B(il2cpp_method_get_param_count) B(il2cpp_method_get_param)
    B(il2cpp_type_get_name) B(il2cpp_resolve_icall) B(il2cpp_object_new) B(il2cpp_class_get_field_from_name) B(il2cpp_field_get_offset)
    B(il2cpp_field_static_get_value) B(il2cpp_gchandle_new) B(il2cpp_class_get_parent) B(il2cpp_free)
    B(il2cpp_class_get_type) B(il2cpp_type_get_object) B(il2cpp_object_get_class) B(il2cpp_class_get_name) B(il2cpp_string_new)
#undef B
    return true;
}
static void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) { void* k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name); if (k) return k; }
    return nullptr;
}
static MethodInfo* FindMethod(void* klass, const char* name, int argc, const char* types = nullptr) {
    for (void* k = klass; k; k = il2cpp_class_get_parent(k)) {
        void* it = nullptr;
        while (MethodInfo* m = il2cpp_class_get_methods(k, &it)) {
            if (strcmp(il2cpp_method_get_name(m), name) != 0) continue;
            if (argc >= 0 && (int)il2cpp_method_get_param_count(m) != argc) continue;
            if (types) {
                std::string sig;
                for (int i = 0; i < argc; i++) { char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, i)); if (i) sig += ","; sig += tn; il2cpp_free(tn); }
                if (sig != types) continue;
            }
            return m;
        }
    }
    return nullptr;
}
static int g_missing = 0;
static MethodInfo* Need(void* klass, const char* kname, const char* name, int argc, const char* types = nullptr) {
    MethodInfo* m = klass ? FindMethod(klass, name, argc, types) : nullptr;
    Log("HOOKMAP %s.%s(%d) %s", kname, name, argc, m ? "found" : "NOT FOUND");
    if (!m) g_missing++;
    return m;
}
static int NeedField(void* klass, const char* kname, const char* name) {
    void* f = klass ? il2cpp_class_get_field_from_name(klass, name) : nullptr;
    int off = f ? (int)il2cpp_field_get_offset(f) : -1;
    Log("HOOKMAP field %s.%s %s (offset 0x%X)", kname, name, f ? "found" : "NOT FOUND", off);
    if (!f) g_missing++;
    return off;
}
template <class F> F P(MethodInfo* m) { return (F)m->methodPointer; }
template <class T> static T& Fld(Il2CppObject* o, int off) { return *(T*)((uint8_t*)o + off); }
static Il2CppObject* TypeOf(void* klass) { return il2cpp_type_get_object(il2cpp_class_get_type(klass)); }
static const char* ClassName(Il2CppObject* o) { return o ? il2cpp_class_get_name(il2cpp_object_get_class(o)) : "null"; }
static Il2CppObject* Keep(Il2CppObject* o) { if (o) il2cpp_gchandle_new(o, false); return o; }

// ---------------------------------------------------------------- game API (found by name in Setup)
static void *kCamera, *kHDCam, *kCanvas, *kVolume, *kGameObject;
static MethodInfo *mCamGetTarget, *mCamSetTarget, *mCamCopyFrom, *mCamSetProj, *mCamGetNear, *mCamGetFar, *mCamSetFov, *mCamSetAspect,
    *mCamGetDepth, *mCamSetDepth, *mCamSetMask, *mCamSetClearFlags, *mCamSetBg, *mBehEnabled, *mBehSetEnabled, *mCompTransform, *mCompGet,
    *mTrPos, *mTrRot, *mTrSetPosRot, *mGoCtor, *mGoAdd, *mObjDontDestroy, *mObjFindAll, *mHDCopyTo, *mRTCtor, *mRTCreate, *mTexNative,
    *mCanvasMode, *mCanvasSetMode, *mCanvasSetCam, *mCanvasSetPlane, *mCanvasRoot, *mTimeTime, *mTimeScale, *mTimeFixedDelta,
    *mTimeMaxDelta, *mTimeFrame, *mVSync, *mSetVSync, *mTargetFps, *mSetTargetFps, *mScreenW, *mScreenH, *mPrefsGetInt,
    *mListEnabledActions, *mActionEnable, *mActionDisable, *mRender;
static MethodInfo *mObjInstantiate, *mCpvSetCam, *mGoInParent, *mCamSetOrtho, *mCamSetOrthoSize, *mCamSetNear, *mCamSetFar;
static void* kGlobal; static int oGlTargetRoot = -1, oGlTargetPoint = -1, oHideRoot = -1;
static std::set<Il2CppObject*> g_cpvCloned;
static MethodInfo *mObjName, *mCanvasGetCam, *mCompGO, *mGoLayer, *mGoActive, *mCanvasOrder; static void* kCPV; static int oCpvGlobal = -1, oCpvPasses = -1, oCpvInject = -1, oCpvCam = -1, oCpvUseCam = -1, oPassName = -1, oPassEnabled = -1;
static MethodInfo* mVolMgrInstance; static int oVolMgrVolumes = -1;
static int oHdClearMode, oHdBg, oHdVolMask, oHdAA, oVolProfile, oProfComps, oCompActive, oListItems = 0x10, oListSize = 0x18;
typedef void (*IssuePluginEvent_t)(void* cb, int id);
static IssuePluginEvent_t g_issue;

static Il2CppObject* GetObj(MethodInfo* m, Il2CppObject* o) { return P<Il2CppObject* (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static float SF(MethodInfo* m) { return P<float (*)(MethodInfo*)>(m)(m); }
static int SI(MethodInfo* m) { return P<int (*)(MethodInfo*)>(m)(m); }
static void SetSI(MethodInfo* m, int v) { P<void (*)(int, MethodInfo*)>(m)(v, m); }
static float IF(MethodInfo* m, Il2CppObject* o) { return P<float (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static int II(MethodInfo* m, Il2CppObject* o) { return P<int (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static bool IB(MethodInfo* m, Il2CppObject* o) { return P<bool (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static void SetIB(MethodInfo* m, Il2CppObject* o, bool v) { P<void (*)(Il2CppObject*, bool, MethodInfo*)>(m)(o, v, m); }
static void SetII(MethodInfo* m, Il2CppObject* o, int v) { P<void (*)(Il2CppObject*, int, MethodInfo*)>(m)(o, v, m); }
static void SetIF(MethodInfo* m, Il2CppObject* o, float v) { P<void (*)(Il2CppObject*, float, MethodInfo*)>(m)(o, v, m); }
static void SetIO(MethodInfo* m, Il2CppObject* o, Il2CppObject* v) { P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(m)(o, v, m); }
typedef void* (*TexNativeIcall_t)(Il2CppObject*);
static TexNativeIcall_t g_texNativeIcall;  // the method is stripped in this game; the engine's internal call is used instead
static void* TexPtr(Il2CppObject* t) { return mTexNative ? P<void* (*)(Il2CppObject*, MethodInfo*)>(mTexNative)(t, mTexNative) : g_texNativeIcall(t); }
static Il2CppObject* Transform(Il2CppObject* c) { return GetObj(mCompTransform, c); }
static Il2CppObject* GetComp(Il2CppObject* c, void* klass) { return P<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mCompGet)(c, TypeOf(klass), mCompGet); }

static Il2CppObject* MakeRT(int w, int h, const char* name) {
    Il2CppObject* rt = Keep(il2cpp_object_new(FindClass("UnityEngine", "RenderTexture")));
    P<void (*)(Il2CppObject*, int, int, int, int, int, MethodInfo*)>(mRTCtor)(rt, w, h, 24, 0 /*ARGB32*/, 2 /*sRGB*/, mRTCtor);
    P<bool (*)(Il2CppObject*, MethodInfo*)>(mRTCreate)(rt, mRTCreate);
    Log("EYES texture %s %dx%d ARGB32 sRGB", name, w, h);
    return rt;
}
static Il2CppObject* NewGameObject(const char* name) {
    Il2CppObject* go = Keep(il2cpp_object_new(kGameObject));
    P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mGoCtor)(go, il2cpp_string_new(name), mGoCtor);
    P<void (*)(Il2CppObject*, MethodInfo*)>(mObjDontDestroy)(go, mObjDontDestroy);
    return go;
}
static Il2CppObject* AddComp(Il2CppObject* go, void* klass) { return Keep(P<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mGoAdd)(go, TypeOf(klass), mGoAdd)); }
static Il2CppArray* FindAll(void* klass) { return P<Il2CppArray* (*)(Il2CppObject*, MethodInfo*)>(mObjFindAll)(TypeOf(klass), mObjFindAll); }

struct Il2CppStr { void* k; void* m; int32_t len; wchar_t c[1]; };
static std::string Str(Il2CppObject* so) {
    if (!so) return "null";
    Il2CppStr* st = (Il2CppStr*)so; std::string o;
    for (int i = 0; i < st->len && i < 120; i++) o += st->c[i] < 128 ? (char)st->c[i] : '?';
    return o;
}
static std::string Name(Il2CppObject* o) { return o && mObjName ? Str(GetObj(mObjName, o)) : "null"; }

// ---------------------------------------------------------------- lab snapshots (render thread readback, raw files)
struct Snap { ID3D11Texture2D* tex; std::string name; };
static std::mutex g_snapMx;
static std::vector<Snap> g_snapQueue;
static void WriteSnap(ID3D11Texture2D* src, const std::string& name) {
    ID3D11Device* dev = nullptr; src->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, src);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) {
            std::wstring dir = g_gameDir + L"AILimitVR\\snaps\\";
            CreateDirectoryW(dir.c_str(), nullptr);
            FILE* f = _wfopen((dir + std::wstring(name.begin(), name.end()) + L".raw").c_str(), L"wb");
            if (f) {
                uint32_t hdr[4] = {d.Width, d.Height, (uint32_t)d.Format, 4};
                fwrite(hdr, 4, 4, f);
                for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t*)map.pData + y * map.RowPitch, 1, d.Width * 4, f);
                fclose(f);
            }
            ctx->Unmap(st, 0);
            Log("SNAP %s %ux%u format %d written", name.c_str(), d.Width, d.Height, (int)d.Format);
        }
        st->Release();
    }
    ctx->Release(); dev->Release();
}
static void __stdcall OnSnapEvent(int) {
    std::vector<Snap> q;
    { std::lock_guard<std::mutex> lk(g_snapMx); q.swap(g_snapQueue); }
    for (auto& s : q) WriteSnap(s.tex, s.name);
}

// ---------------------------------------------------------------- state
static bool g_vrOn = true;           // lab: "vr 0" stops eye rendering (flat game)
static bool g_flatToo = false;       // lab: "flat 1" keeps the game's own flat render while VR runs
static bool g_wantSnap = false, g_wantInfo = false, g_wantDumpUi = false;
static bool g_eyeCheck = false;      // lab: both eyes at the game camera's own pose (zero-difference control)
static int g_lastFrame = -1, g_framesSeen = 0, g_posedFrame = -1;
static Il2CppObject *g_eyeCam[2], *g_eyeRT[2], *g_hudCam, *g_hudRT, *g_gameCam;
static int g_eyeW = 0, g_eyeH = 0;
static int kHudW = 1920, kHudH = 1080;  // the game window's size, read at VR start
static uint64_t g_snapCount = 0;
static uint32_t g_renderCalls = 0, g_eyeFrames = 0, g_flatSkipped = 0, g_scanCount = 0;
static bool g_vrStarted = false;
static double g_capNext = 0, g_capWaitSum = 0, g_capWaitMax = 0, g_hookCpuSum = 0, g_hookCpuMax = 0, g_lastFrameT = 0, g_frameMax = 0, g_frameSum = 0;
static uint32_t g_capped = 0, g_late = 0, g_frames = 0;
static double g_bestPeriod = 0, g_capPeriod = 0, g_lowSince = -1;
static double g_scanMsMax = 0;
static uint32_t g_netBlocked = 0, g_netAllowed = 0;

// The game's input actions are switched off while the menu is open, and the same ones switched back on afterwards.
static std::vector<Il2CppObject*> g_offActions;
static void BlockGameInput(bool block) {
    if (!mListEnabledActions || !mActionEnable || !mActionDisable) return;
    if (block) {
        Il2CppObject* list = P<Il2CppObject* (*)(MethodInfo*)>(mListEnabledActions)(mListEnabledActions);
        g_offActions.clear();
        if (list) {
            Il2CppArray* items = Fld<Il2CppArray*>(list, oListItems); int n = Fld<int>(list, oListSize);
            for (int i = 0; i < n; i++) { Il2CppObject* a = (Il2CppObject*)items->data[i]; Keep(a); g_offActions.push_back(a); }
            for (auto a : g_offActions) P<void (*)(Il2CppObject*, MethodInfo*)>(mActionDisable)(a, mActionDisable);
        }
        Log("INPUT %zu game input actions switched off while the menu is open", g_offActions.size());
    } else {
        for (auto a : g_offActions) P<void (*)(Il2CppObject*, MethodInfo*)>(mActionEnable)(a, mActionEnable);
        Log("INPUT %zu game input actions switched back on", g_offActions.size());
        g_offActions.clear();
    }
}

// ---------------------------------------------------------------- upscaler per eye (HDRP's own DLSS)
static int oHdAllowDyn = -1, oHdAllowDlss = -1, oHdDlssCustomQ = -1, oHdDlssQ = -1, oHdDlssCustomAttr = -1, oHdDlssOptimal = -1;
static int oAssetSettings = -1, oDynRes = -1, oDynEnabled = -1, oDynDlss = -1;
static MethodInfo *mCamSetAllowDyn, *mDrhInstance, *mDrhScale;
static char g_dlssStatus[160] = "not applied yet";
static const char* DlssStatus() { return g_dlssStatus; }
static void ApplyUpscaler(const char* why) {
    if (!g_eyeCam[0] || oHdAllowDlss < 0) { sprintf_s(g_dlssStatus, "waiting for the eye cameras"); return; }
    int u = g_set.upscaler;
    bool dlss = u >= 1;
    // DLSS in HDRP needs dynamic resolution and DLSS switched on in the render pipeline asset (the game's DLAA uses it).
    Il2CppObject* asset = nullptr;
    void* kGS = FindClass("UnityEngine.Rendering", "GraphicsSettings");
    MethodInfo* mCur = kGS ? FindMethod(kGS, "get_currentRenderPipeline", 0) : nullptr;
    if (mCur) asset = P<Il2CppObject* (*)(MethodInfo*)>(mCur)(mCur);
    bool assetDyn = false, assetDlss = false;
    if (asset && oAssetSettings >= 0) {
        uint8_t* dyn = (uint8_t*)asset + oAssetSettings + oDynRes;
        assetDyn = dyn[oDynEnabled] != 0; assetDlss = dyn[oDynDlss] != 0;
        if (dlss && (!assetDyn || !assetDlss)) { dyn[oDynEnabled] = 1; dyn[oDynDlss] = 1; Log("UPSCALER render pipeline asset: dynamic resolution %d -> 1, DLSS %d -> 1", assetDyn, assetDlss); }
    }
    static const uint32_t q[6] = {0, 5, 2, 1, 0, 3};  // TAA, DLAA, MaximumQuality, Balanced, MaximumPerformance, UltraPerformance (NVIDIA enum)
    for (int e = 0; e < 2; e++) {
        Il2CppObject* hd = GetComp(g_eyeCam[e], kHDCam); if (!hd) continue;
        Fld<bool>(hd, oHdAllowDlss) = dlss; Fld<bool>(hd, oHdAllowDyn) = dlss;
        Fld<bool>(hd, oHdDlssCustomQ) = true; Fld<uint32_t>(hd, oHdDlssQ) = q[u];
        Fld<bool>(hd, oHdDlssCustomAttr) = true; Fld<bool>(hd, oHdDlssOptimal) = true;
        if (u == 0) Fld<int>(hd, oHdAA) = 2;  // TAA
        P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mCamSetAllowDyn)(g_eyeCam[e], dlss, mCamSetAllowDyn);
    }
    static const char* names[] = {"TAA", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance"};
    sprintf_s(g_dlssStatus, "%s", names[u]);
    Log("UPSCALER %s (%s): eyes allowDLSS %d quality %u, asset dynamic resolution was %d DLSS was %d", names[u], why, dlss, q[u], assetDyn, assetDlss);
}
static float DrsScale() {
    if (!mDrhInstance || !mDrhScale) return -1;
    Il2CppObject* h = P<Il2CppObject* (*)(MethodInfo*)>(mDrhInstance)(mDrhInstance);
    return h ? P<float (*)(Il2CppObject*, MethodInfo*)>(mDrhScale)(h, mDrhScale) : -1;
}

// ---------------------------------------------------------------- lock-on marker lift (Steve, build 2: a touch too low)
static Il2CppObject* g_markerTr; static Vec3 g_markerWritten{1e9f, 0, 0};
static MethodInfo* mTrSetPos;
static void LiftMarker() {
    if (g_set.lockOnLift == 0 || !kGlobal) return;
    try {
        if (!g_markerTr) {
            Il2CppArray* gl = FindAll(kGlobal);
            if (gl && gl->max_length) { Il2CppObject* tp = Fld<Il2CppObject*>((Il2CppObject*)gl->data[0], oGlTargetPoint); if (tp) g_markerTr = Keep(tp); }
            if (!g_markerTr) return;
        }
        Vec3 p = P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrPos)(g_markerTr, mTrPos);
        if (p.x == g_markerWritten.x && p.y == g_markerWritten.y && p.z == g_markerWritten.z) return;  // the game did not move it since our lift
        p.y += g_set.lockOnLift;
        P<void (*)(Il2CppObject*, Vec3, MethodInfo*)>(mTrSetPos)(g_markerTr, p, mTrSetPos);
        g_markerWritten = P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrPos)(g_markerTr, mTrPos);
    } catch (...) { g_markerTr = nullptr; }
}

#include "Menu.inc"

static int Pref(const char* key, int def) { return P<int (*)(Il2CppObject*, int, MethodInfo*)>(mPrefsGetInt)(il2cpp_string_new(key), def, mPrefsGetInt); }
static void LogGameSet(const char* when) {
    Log("GAMESET %s: Unity vSyncCount %d, targetFrameRate %d, screen %dx%d; game VSYNC %d, LOCK FPS %d, quality %d, textures %d, shadows %d, fog %d, AO %d, motion blur %d, AA %d (prefs)",
        when, SI(mVSync), SI(mTargetFps), SI(mScreenW), SI(mScreenH), Pref("GameOptions.Graphics.vSyncCount", -9), Pref("GameOptions.Graphics.TargetFrameRate", -9),
        Pref("GameOptions.Graphics.qualityMode", -9), Pref("GameOptions.Graphics.textureQuality", -9), Pref("GameOptions.Graphics.shadowsQuality", -9),
        Pref("GameOptions.Graphics.fogQuality", -9), Pref("GameOptions.Graphics.hbaoQuality", -9), Pref("GameOptions.Graphics.motionBlur", -9), Pref("GameOptions.Graphics.Antialiasing", -9));
}
// VSync and the frame limit are always off while VR runs: the mod paces the game at the headset's rate.
static void KeepPacingOff(const char* when) {
    bool changed = false;
    if (SI(mVSync) != 0) { SetSI(mSetVSync, 0); changed = true; }
    if (SI(mTargetFps) != -1) { SetSI(mSetTargetFps, -1); changed = true; }
    if (changed) LogGameSet(when);
}

// ---------------------------------------------------------------- frame cap at the headset's real rate (Ori WotW VR)
static void Cap(double periodSec) {
    double now = Now();
    if (periodSec < 1.0 / 250 || periodSec > 1.0 / 30) periodSec = g_bestPeriod;
    if (periodSec <= 0) return;
    if (g_bestPeriod == 0 || periodSec < g_bestPeriod * 0.98) { g_bestPeriod = periodSec; g_capPeriod = periodSec; g_lowSince = -1; Log("CAP headset rate %.1f Hz: the mod caps the game there", 1.0 / periodSec); }
    else if (periodSec > g_bestPeriod * 1.02) {
        double ratio = periodSec / g_bestPeriod;
        bool smoothing = fabs(ratio - 2) < 0.05 || fabs(ratio - 1.5) < 0.05 || fabs(ratio - 3) < 0.05;
        if (g_lowSince < 0) g_lowSince = now;
        if (!smoothing && now - g_lowSince > 20 && fabs(g_capPeriod - periodSec) > 1e-5) { g_capPeriod = periodSec; g_bestPeriod = periodSec; Log("CAP the headset now runs at %.1f Hz steadily: cap follows", 1.0 / periodSec); }
    } else g_lowSince = -1;
    if (g_capPeriod <= 0) return;
    if (g_capNext == 0 || now - g_capNext > g_capPeriod) g_capNext = now;
    double t0 = now;
    while (Now() < g_capNext) { if (g_capNext - Now() > 0.0015) Sleep(1); else YieldProcessor(); }
    double w = (Now() - t0) * 1000;
    g_capWaitSum += w; if (w > g_capWaitMax) g_capWaitMax = w; g_capped++;
    g_capNext += g_capPeriod;
}

// ---------------------------------------------------------------- scene scan: effects off, canvases to the HUD camera
// Every 2 s while VR runs (scenes load new volumes and canvases). Its cost is logged (SCAN max ms).
static std::set<Il2CppObject*> g_offComps;
static const char* kOff[] = {"FilmGrain", "Bloom", "MotionBlur", "RadialBlur", "HBAO", "ScreenSpaceAmbientOcclusion", "ScreenSpaceReflection", "ChromaticAberration"};
static double g_scanVolMax = 0, g_scanCanvasMax = 0;
static void Scan(int part) {
    double t0 = Now();
    // The volumes HDRP knows about, from VolumeManager.instance.m_Volumes (FindObjectsOfType cost about 6 ms).
    Il2CppArray* vols = nullptr; int nv = 0;
    if (part == 0) {
        Il2CppObject* vm = P<Il2CppObject* (*)(MethodInfo*)>(mVolMgrInstance)(mVolMgrInstance);
        Il2CppObject* vl = vm ? Fld<Il2CppObject*>(vm, oVolMgrVolumes) : nullptr;
        if (vl) { vols = Fld<Il2CppArray*>(vl, oListItems); nv = Fld<int>(vl, oListSize); }
    }
    for (int i = 0; vols && i < nv; i++) {
        Il2CppObject* v = (Il2CppObject*)vols->data[i]; if (!v) continue;
        Il2CppObject* prof = Fld<Il2CppObject*>(v, oVolProfile); if (!prof) continue;
        Il2CppObject* list = Fld<Il2CppObject*>(prof, oProfComps); if (!list) continue;
        Il2CppArray* items = Fld<Il2CppArray*>(list, oListItems); int n = Fld<int>(list, oListSize);
        for (int k = 0; k < n; k++) {
            Il2CppObject* c = (Il2CppObject*)items->data[k]; if (!c) continue;
            const char* cn = ClassName(c);
            for (const char* off : kOff) if (!strcmp(cn, off)) {
                if (Fld<bool>(c, oCompActive)) { Fld<bool>(c, oCompActive) = false; if (!g_offComps.count(c)) { g_offComps.insert(c); Log("EFFECTS %s off in a volume profile (VR-unfriendly)", cn); } }
            }
        }
    }
    if (part == 0) { double ms = (Now() - t0) * 1000; if (ms > g_scanVolMax) g_scanVolMax = ms; }
    if (part == 1 && g_hudCam) {
        Il2CppArray* cvs = FindAll(kCanvas);
        int moved = 0;
        for (uintptr_t i = 0; cvs && i < cvs->max_length; i++) {
            Il2CppObject* c = (Il2CppObject*)cvs->data[i]; if (!c || !IB(mCanvasRoot, c)) continue;
            int mode = II(mCanvasMode, c);
            if (mode == 0) { SetII(mCanvasSetMode, c, 1); SetIO(mCanvasSetCam, c, g_hudCam); SetIF(mCanvasSetPlane, c, 1.0f); moved++; }
            else if (mode == 1 && g_gameCam && GetObj(mCanvasGetCam, c) == g_gameCam) {
                // Screen Space Camera bound to the game's camera, which VR leaves out: the HUD camera draws it instead.
                SetIO(mCanvasSetCam, c, g_hudCam); SetIF(mCanvasSetPlane, c, 1.0f); moved++;
                Log("HUD canvas \"%s\" was drawn by the game camera: moved to the HUD camera", Name(c).c_str());
            }
        }
        if (moved) Log("HUD %d overlay canvas(es) moved to the HUD camera (of %d canvases)", moved, cvs ? (int)cvs->max_length : 0);
        double ms = (Now() - t0) * 1000; if (ms > g_scanCanvasMax) g_scanCanvasMax = ms;
    }
    // (Custom pass volumes: all global in this game, dumpui 2026-11-20; a per-frame search cost 6 ms, so none here.)
    double ms = (Now() - t0) * 1000; if (ms > g_scanMsMax) g_scanMsMax = ms; g_scanCount++;
}

// ---------------------------------------------------------------- periodic log block (every 5 s)
static double g_statT0 = -1; static float g_statGame0 = 0; static int g_statFrames0 = 0;
static void Periodic() {
    double t = Now();
    float game = SF(mTimeTime); int frame = SI(mTimeFrame);
    if (g_statT0 < 0) { g_statT0 = t; g_statGame0 = game; g_statFrames0 = frame; return; }
    double dt = t - g_statT0;
    if (dt < 5.0) return;
    int frames = frame - g_statFrames0;
    double speed = (game - g_statGame0) / dt;
    aixr::Views v; aixr::GetViews(&v);
    double hz = v.periodSec > 0 ? 1.0 / v.periodSec : 0;
    Log("SPEED %.1f s: game speed %.3f, %.1f frames/s, headset %.1f Hz (cap %.1f Hz), timeScale %.3f, fixed step %.2f ms, maxDelta %.3f",
        dt, speed, frames / dt, hz, g_capPeriod > 0 ? 1.0 / g_capPeriod : 0, SF(mTimeScale), SF(mTimeFixedDelta) * 1000, SF(mTimeMaxDelta));
    if (g_vrStarted && fabs(speed - 1.0) > 0.05 && SF(mTimeScale) > 0.99) Log("SPEED WARNING: game time runs at %.2f of real time", speed);
    aixr::Stats s; aixr::TakeStats(&s);
    double cpu = g_frames ? (g_frameSum - g_capWaitSum) / g_frames : 0;
    Log("PERF frame avg %.2f ms max %.2f ms | CPU (frame minus cap wait) avg %.2f ms | GPU frame avg %.2f max %.2f ms (%u samples) | cap wait avg %.2f max %.2f ms | mod hook CPU avg %.2f max %.2f ms | %u eye frames, %u render calls, flat skipped %u, late %u | scan max %.2f ms (volumes %.2f, canvases %.2f) | upscaler %s, resolution scale now %.2f",
        frames ? dt * 1000 / frames : 0, g_frameMax, cpu, s.gpuMsAvg, s.gpuMsMax, s.gpuSamples, g_capped ? g_capWaitSum / g_capped : 0, g_capWaitMax,
        g_eyeFrames ? g_hookCpuSum / g_eyeFrames : 0, g_hookCpuMax, g_eyeFrames, g_renderCalls, g_flatSkipped, g_late, g_scanMsMax, g_scanVolMax, g_scanCanvasMax, g_dlssStatus, DrsScale()); g_scanVolMax = g_scanCanvasMax = 0;
    Log("XRSTAT state %d session %d focused %d, xr frames %u (no new image %u, layers sent %u), queued %u copied %u skipped busy %u no slot %u, copy max %.2f ms, xrWaitFrame avg %.2f max %.2f ms, xrEndFrame avg %.2f max %.2f ms, layer %d",
        aixr::GetState(), v.sessionState, v.focused, s.xrFrames, s.xrNoNewImage, s.layersSent, s.queued, s.copied, s.skippedBusy, s.skippedNoSlots, s.copyMsMax,
        s.waitMsAvg, s.waitMsMax, s.endMsAvg, s.endMsMax, aixr::LayerInUse());
    Log("INPUT pad reads %u, menu open %d, blocking the game %d | NET blocked %u allowed (local) %u", menu::g_padReads, menu::g_open, menu::g_block, g_netBlocked, g_netAllowed);
    menu::g_padReads = 0;
    g_capWaitSum = g_capWaitMax = g_hookCpuSum = g_hookCpuMax = g_frameMax = g_frameSum = g_scanMsMax = 0; g_capped = g_eyeFrames = g_renderCalls = g_flatSkipped = g_late = g_frames = 0;
    g_statT0 = t; g_statGame0 = game; g_statFrames0 = frame;
    if (g_vrStarted) KeepPacingOff("the game turned VSync or a frame cap on during VR. Turned off again: the mod paces the game");
}

static void PollCommands() {
    std::wstring path = g_gameDir + L"vrlab_command.txt";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf(); f.close();
    DeleteFileW(path.c_str());
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        Log("CMD %s", line.c_str());
        std::istringstream ls(line); std::string c; ls >> c;
        if (c == "info") g_wantInfo = true;
        else if (c == "snap") g_wantSnap = true;
        else if (c == "xrsnap") { static int n = 0; char pth[MAX_PATH]; sprintf_s(pth, "%lsAILimitVR/snaps/xr%d.raw", g_gameDir.c_str(), ++n); aixr::SnapComposed(pth); }
        else if (c == "eyecheck") { int n; ls >> n; g_eyeCheck = n != 0; }
        else if (c == "vr") { int n; ls >> n; g_vrOn = n != 0; }
        else if (c == "flat") { int n; ls >> n; g_flatToo = n != 0; }
        else if (c == "recenter") aixr::Recenter();
        else if (c == "gamesettings") LogGameSet("asked");
        else if (c == "dumpui") g_wantDumpUi = true;
        else if (c == "pitch") { int v; ls >> v; g_set.decoupledPitch = v; Log("PITCH decoupled %d (lab)", v); }
        else if (c == "menu") { menu::g_open = !menu::g_open; Log("MENU %s (lab)", menu::g_open ? "opened" : "closed"); }
        else if (c == "hud") { int n; ls >> n; g_set.hudMode = n; menu::PushHud(); }
        else Log("CMD unknown");
    }
}

// ---------------------------------------------------------------- eyes
// Unity's projection matrix (OpenGL style) from the eye's field of view tangents.
static Mat4 Projection(const float fov[4], float n, float f) {
    float l = tanf(fov[0]), r = tanf(fov[1]), u = tanf(fov[2]), d = tanf(fov[3]);
    Mat4 p{};
    M(p, 0, 0) = 2 / (r - l); M(p, 0, 2) = (r + l) / (r - l);
    M(p, 1, 1) = 2 / (u - d); M(p, 1, 2) = (u + d) / (u - d);
    M(p, 2, 2) = -(f + n) / (f - n); M(p, 2, 3) = -2 * f * n / (f - n);
    M(p, 3, 2) = -1;
    return p;
}
static Vec3 QRot(Quat q, Vec3 v) {
    Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z), v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
static Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static aixr::Views g_usedViews;
static void CopyGameCamera(Il2CppObject* game) {
    Il2CppObject* ghd = GetComp(game, kHDCam);
    for (int e = 0; e < 2; e++) {
        P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mCamCopyFrom)(g_eyeCam[e], game, mCamCopyFrom);
        SetIO(mCamSetTarget, g_eyeCam[e], g_eyeRT[e]);
        SetIF(mCamSetDepth, g_eyeCam[e], IF(mCamGetDepth, game) - 1);
        Il2CppObject* hd = GetComp(g_eyeCam[e], kHDCam);
        if (ghd && hd) P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mHDCopyTo)(ghd, hd, mHDCopyTo);
    }
    Log("EYES settings copied from the game camera %p (HD data %s), near %.3f far %.0f", game, ghd ? "copied" : "missing", IF(mCamGetNear, game), IF(mCamGetFar, game));
}
static void MakeEyes(Il2CppObject* game) {
    g_eyeW = aixr::EyeWidth(); g_eyeH = aixr::EyeHeight();
    for (int e = 0; e < 2; e++) {
        g_eyeRT[e] = MakeRT(g_eyeW, g_eyeH, e ? "AILVR_R" : "AILVR_L");
        Il2CppObject* go = NewGameObject(e ? "AILVR_EyeR" : "AILVR_EyeL");
        g_eyeCam[e] = AddComp(go, kCamera); AddComp(go, kHDCam);
    }
    CopyGameCamera(game);
    ApplyUpscaler("start");
    // HUD camera: renders the game's overlay canvases (layer 5, UI) into a see-through texture; no volumes, no AA.
    g_hudRT = MakeRT(kHudW, kHudH, "AILVR_HUD");
    Il2CppObject* go = NewGameObject("AILVR_HudCam");
    g_hudCam = AddComp(go, kCamera); Il2CppObject* hd = AddComp(go, kHDCam);
    SetII(mCamSetClearFlags, g_hudCam, 2 /*SolidColor*/);
    P<void (*)(Il2CppObject*, Color, MethodInfo*)>(mCamSetBg)(g_hudCam, Color{0, 0, 0, 0}, mCamSetBg);
    SetII(mCamSetMask, g_hudCam, 1 << 5); SetIF(mCamSetDepth, g_hudCam, 10); SetIO(mCamSetTarget, g_hudCam, g_hudRT);
    Fld<int>(hd, oHdClearMode) = 1 /*Color*/; Fld<Color>(hd, oHdBg) = Color{0, 0, 0, 0}; Fld<int>(hd, oHdVolMask) = 0; Fld<int>(hd, oHdAA) = 0;
    // The game places some overlay elements (lock-on marker, damage numbers) by setting their position to screen
    // pixels, which only works on an overlay canvas. An orthographic HUD camera looking at the rectangle (0,0)-(W,H)
    // makes world units equal screen pixels, so those elements and the normal layout both land where the game meant.
    P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mCamSetOrtho)(g_hudCam, true, mCamSetOrtho);
    SetIF(mCamSetOrthoSize, g_hudCam, kHudH * 0.5f);
    SetIF(mCamSetNear, g_hudCam, 0.01f); SetIF(mCamSetFar, g_hudCam, 100000.0f);
    P<void (*)(Il2CppObject*, Vec3, Quat, MethodInfo*)>(mTrSetPosRot)(Transform(g_hudCam), Vec3{kHudW * 0.5f, kHudH * 0.5f, -1.0f}, Quat{0, 0, 0, 1}, mTrSetPosRot);
    Log("EYES two eye cameras %dx%d and the HUD camera %dx%d made (HUD camera orthographic, 1 unit = 1 screen pixel)", g_eyeW, g_eyeH, kHudW, kHudH);
}

// Called once per frame before the frame's first render: both eyes from the game camera's pose now plus the head pose.
static void PoseEyes(Il2CppObject* game) {
    aixr::Views v; aixr::GetViews(&v);
    g_usedViews = v;
    Il2CppObject* tr = Transform(game);
    Vec3 cp = P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrPos)(tr, mTrPos);
    Quat cr = P<Quat (*)(Il2CppObject*, MethodInfo*)>(mTrRot)(tr, mTrRot);
    if (g_set.decoupledPitch) {
        // Decoupled pitch: keep only the game camera's heading (yaw). Its pitch and roll (stick tilt, lock-on tilt,
        // shake roll) no longer tilt the world; the player looks up and down with the head. The camera position is kept.
        Vec3 f = QRot(cr, Vec3{0, 0, 1});
        float yaw = atan2f(f.x, f.z);
        cr = Quat{0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    }
    float n = IF(mCamGetNear, game), f = IF(mCamGetFar, game);
    bool tracked = v.valid && v.recenters && !g_eyeCheck;
    for (int e = 0; e < 2; e++) {
        Vec3 lp{0, 0, 0}; Quat lr{0, 0, 0, 1};
        if (tracked) { lp = {v.local[e][0], v.local[e][1], -v.local[e][2]}; lr = {-v.localRot[e][0], -v.localRot[e][1], v.localRot[e][2], v.localRot[e][3]}; }
        else if (!g_eyeCheck) lp = {e ? 0.032f : -0.032f, 0, 0};
        lp = {lp.x * g_set.worldScale, lp.y * g_set.worldScale, lp.z * g_set.worldScale};
        Vec3 off = QRot(cr, lp);
        Vec3 ep{cp.x + off.x, cp.y + off.y, cp.z + off.z};
        Quat er = QMul(cr, lr);
        Il2CppObject* et = Transform(g_eyeCam[e]);
        P<void (*)(Il2CppObject*, Vec3, Quat, MethodInfo*)>(mTrSetPosRot)(et, ep, er, mTrSetPosRot);
        float fv[4] = {v.fov[e][0], v.fov[e][1], v.fov[e][2], v.fov[e][3]};
        if (!v.valid || g_eyeCheck) { float t = 0.9f; fv[0] = -t; fv[1] = t; fv[2] = t; fv[3] = -t; }  // no headset view yet: 103 degrees square
        P<void (*)(Il2CppObject*, Mat4, MethodInfo*)>(mCamSetProj)(g_eyeCam[e], Projection(fv, n, f), mCamSetProj);
        SetIF(mCamSetFov, g_eyeCam[e], (fv[2] - fv[3]) * 57.29578f);
        SetIF(mCamSetAspect, g_eyeCam[e], (tanf(fv[1]) - tanf(fv[0])) / (tanf(fv[2]) - tanf(fv[3])));
    }
}

// HDRenderPipeline.Render(ScriptableRenderContext, List<Camera>)
typedef void (*Render_t)(Il2CppObject* self, void* ctx, Il2CppObject* cams, MethodInfo* m);
static Render_t oRender;
static void FrameStart(int frame);

static void hkRender(Il2CppObject* self, void* ctx, Il2CppObject* cams, MethodInfo* m) {
    g_renderCalls++;
    int frame = SI(mTimeFrame);
    if (frame != g_lastFrame) { g_lastFrame = frame; FrameStart(frame); }
    bool vrRunning = g_vrOn && aixr::GetState() == aixr::Running && aixr::EyeWidth() > 0;
    static bool listChecked = false;
    if (!listChecked && cams) {
        // List<Camera> field offsets from the real (closed) list class; the open generic List`1 reports 0.
        listChecked = true;
        void* k = il2cpp_object_get_class(cams);
        void* fi = il2cpp_class_get_field_from_name(k, "_items"); void* fs = il2cpp_class_get_field_from_name(k, "_size");
        int oi = fi ? (int)il2cpp_field_get_offset(fi) : 0, os = fs ? (int)il2cpp_field_get_offset(fs) : 0;
        if (oi > 0) oListItems = oi; if (os > 0) oListSize = os;
        Log("HOOKMAP %s _items 0x%X _size 0x%X (used 0x%X 0x%X), %d camera(s) in the first call", il2cpp_class_get_name(k), oi, os, oListItems, oListSize, Fld<int>(cams, oListSize));
    }
    if (!vrRunning || !cams) { oRender(self, ctx, cams, m); return; }
    double t0 = Now();
    Il2CppArray* items = Fld<Il2CppArray*>(cams, oListItems); int n = Fld<int>(cams, oListSize);
    // The game camera: the first enabled camera in this call that draws to the screen and is not ours.
    Il2CppObject* game = nullptr; int gameIdx = -1; bool hasEyes = false;
    for (int i = 0; i < n; i++) {
        Il2CppObject* c = (Il2CppObject*)items->data[i];
        if (c == g_eyeCam[0] || c == g_eyeCam[1]) { hasEyes = true; continue; }
        if (c == g_hudCam || !c) continue;
        if (!game && !GetObj(mCamGetTarget, c)) { game = c; gameIdx = i; }
    }
    if (game) {
        if (!g_eyeCam[0]) MakeEyes(game);
        else if (game != g_gameCam) { CopyGameCamera(game); ApplyUpscaler("game camera changed"); }
        g_gameCam = game;
    }
    if (g_gameCam && g_posedFrame != frame) { g_posedFrame = frame; LiftMarker(); PoseEyes(g_gameCam); }
    // Leave the game's own flat render out while VR runs.
    bool removed = false;
    if (gameIdx >= 0 && !g_flatToo) {
        for (int i = gameIdx; i < n - 1; i++) items->data[i] = items->data[i + 1];
        Fld<int>(cams, oListSize) = n - 1; removed = true; g_flatSkipped++;
    }
    double mid = Now();
    oRender(self, ctx, cams, m);
    double t1 = Now();
    if (removed) { for (int i = n - 1; i > gameIdx; i--) items->data[i] = items->data[i - 1]; items->data[gameIdx] = game; Fld<int>(cams, oListSize) = n; }
    if (hasEyes) {
        void* l = TexPtr(g_eyeRT[0]); void* r = TexPtr(g_eyeRT[1]); void* h = g_hudRT ? TexPtr(g_hudRT) : nullptr;
        aixr::QueueFrame(l, r, h, g_usedViews, (uint32_t)frame);
        g_issue(aixr::RenderEventFunc(), 1);
        g_eyeFrames++;
        if (g_wantSnap) {
            g_wantSnap = false; g_snapCount++;
            const aixr::Views& v = g_usedViews;
            Log("EYES snap %llu frame %d, eyecheck %d, head L (%.3f %.3f %.3f) R (%.3f %.3f %.3f)", (unsigned long long)g_snapCount, frame, g_eyeCheck,
                v.local[0][0], v.local[0][1], v.local[0][2], v.local[1][0], v.local[1][1], v.local[1][2]);
            std::lock_guard<std::mutex> lk(g_snapMx);
            char nm[64];
            sprintf_s(nm, "s%llu_f%d_L", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)l, nm});
            sprintf_s(nm, "s%llu_f%d_R", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)r, nm});
            if (h) { sprintf_s(nm, "s%llu_f%d_HUD", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)h, nm}); }
            g_issue((void*)&OnSnapEvent, 2);
        }
    }
    double ms = ((mid - t0) + (Now() - t1)) * 1000;  // the mod's own CPU time in this hook (not the game's render)
    g_hookCpuSum += ms; if (ms > g_hookCpuMax) g_hookCpuMax = ms;
}

// Lab: every canvas and every HDRP custom pass volume, with what draws it.
static void DumpUi() {
    Il2CppArray* cvs = FindAll(kCanvas);
    for (uintptr_t i = 0; cvs && i < cvs->max_length; i++) {
        Il2CppObject* c = (Il2CppObject*)cvs->data[i]; if (!c) continue;
        Il2CppObject* go = GetObj(mCompGO, c); Il2CppObject* cam = GetObj(mCanvasGetCam, c);
        Log("UIDUMP canvas \"%s\" root %d mode %d camera \"%s\"%s layer %d order %d active %d", Name(c).c_str(), IB(mCanvasRoot, c), II(mCanvasMode, c), Name(cam).c_str(),
            cam == g_gameCam ? " (the game camera)" : cam == g_hudCam ? " (our HUD camera)" : "", II(mGoLayer, go), II(mCanvasOrder, c), IB(mGoActive, go));
    }
    Il2CppArray* cp = kCPV ? FindAll(kCPV) : nullptr;
    for (uintptr_t i = 0; cp && i < cp->max_length; i++) {
        Il2CppObject* v = (Il2CppObject*)cp->data[i]; if (!v) continue;
        Il2CppObject* cam = Fld<Il2CppObject*>(v, oCpvCam);
        std::string passes;
        Il2CppObject* list = Fld<Il2CppObject*>(v, oCpvPasses);
        if (list) { Il2CppArray* it = Fld<Il2CppArray*>(list, oListItems); int n = Fld<int>(list, oListSize);
            for (int k = 0; k < n; k++) { Il2CppObject* ps = (Il2CppObject*)it->data[k]; if (!ps) continue;
                passes += std::string(ClassName(ps)) + "(" + Str(Fld<Il2CppObject*>(ps, oPassName)) + (Fld<bool>(ps, oPassEnabled) ? "" : ", off") + ") "; } }
        Log("UIDUMP custom pass volume \"%s\" global %d useTargetCamera %d camera \"%s\"%s injection %d layer %d: %s", Name(v).c_str(), Fld<bool>(v, oCpvGlobal), Fld<bool>(v, oCpvUseCam),
            Name(cam).c_str(), cam == g_gameCam ? " (the game camera)" : "", Fld<int>(v, oCpvInject), II(mGoLayer, GetObj(mCompGO, v)), passes.c_str());
    }
    // The game's lock-on marker: Global.TargetPointUIRoot (HideUIElement.Root) and Global._TargetPoint.
    Il2CppArray* gl = kGlobal ? FindAll(kGlobal) : nullptr;
    for (uintptr_t i = 0; gl && i < gl->max_length; i++) {
        Il2CppObject* g = (Il2CppObject*)gl->data[i]; if (!g) continue;
        Il2CppObject* hide = Fld<Il2CppObject*>(g, oGlTargetRoot);
        Il2CppObject* rootGo = hide ? Fld<Il2CppObject*>(hide, oHideRoot) : nullptr;
        Il2CppObject* tp = Fld<Il2CppObject*>(g, oGlTargetPoint);
        Il2CppObject* objs[2] = {rootGo, tp ? GetObj(mCompGO, tp) : nullptr};
        const char* what[2] = {"TargetPointUIRoot.Root", "_TargetPoint"};
        for (int k = 0; k < 2; k++) {
            Il2CppObject* go = objs[k]; if (!go) { Log("UIDUMP Global.%s null", what[k]); continue; }
            Il2CppObject* tr = P<Il2CppObject* (*)(Il2CppObject*, MethodInfo*)>(FindMethod(kGameObject, "get_transform", 0))(go, FindMethod(kGameObject, "get_transform", 0));
            Vec3 pos = P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrPos)(tr, mTrPos);
            Il2CppObject* cv = mGoInParent ? P<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mGoInParent)(go, TypeOf(kCanvas), mGoInParent) : nullptr;
            Log("UIDUMP Global.%s \"%s\" active %d layer %d pos (%.2f %.2f %.2f), canvas above it \"%s\" mode %d", what[k], Name(go).c_str(), IB(mGoActive, go), II(mGoLayer, go),
                pos.x, pos.y, pos.z, Name(cv).c_str(), cv ? II(mCanvasMode, cv) : -1);
        }
    }
    Log("UIDUMP %d canvases, %d custom pass volumes", cvs ? (int)cvs->max_length : 0, cp ? (int)cp->max_length : 0);
}

static void FrameStart(int frame) {
    g_framesSeen++;
    double now = Now();
    if (g_lastFrameT > 0) { double ft = (now - g_lastFrameT) * 1000; g_frameSum += ft; g_frames++; if (ft > g_frameMax) g_frameMax = ft; if (g_capPeriod > 0 && ft > g_capPeriod * 1500) g_late++; }
    g_lastFrameT = now;
    if ((g_framesSeen % 15) == 0) PollCommands();
    menu::Update(now);
    Periodic();
    if (!g_vrStarted && g_vrOn) {
        g_vrStarted = true;
        LogGameSet("at start");
        KeepPacingOff("at VR start: VSync and the frame limit off, the mod paces the game");
        kHudW = SI(mScreenW); kHudH = SI(mScreenH); if (kHudW < 640 || kHudH < 360) { kHudW = 1920; kHudH = 1080; }
        aixr::Configure(g_set.picture, g_set.resolution, kHudW, kHudH);
        menu::PushHud();
        aixr::Start();
        Log("VR starting: resolution %.2f, picture %d, HUD mode %d", g_set.resolution, g_set.picture, g_set.hudMode);
    }
    aixr::Views v; aixr::GetViews(&v);
    bool running = g_vrOn && aixr::GetState() == aixr::Running;
    if (running && (g_framesSeen % 120) < 2 && g_framesSeen > 2) { try { Scan(g_framesSeen % 120); } catch (...) { Log("SCAN failed (game exception)"); } }
    if (running) Cap(v.periodSec);
    if (running) g_issue(aixr::RenderEventFunc(), 3);  // GPU timestamp: start of this frame's rendering
    if (g_wantDumpUi) { g_wantDumpUi = false; try { DumpUi(); } catch (...) { Log("UIDUMP failed (game exception)"); } }
    if (g_wantInfo) { g_wantInfo = false; LogGameSet("asked"); Log("INFO frame %d, eyes %dx%d, game camera %p, recenters %u", frame, g_eyeW, g_eyeH, g_gameCam, v.recenters); }
}

// ---------------------------------------------------------------- offline: the game's internet connections are refused
typedef int(WSAAPI* connect_t)(SOCKET, const sockaddr*, int);
static connect_t oConnect;
static bool IsLocal(const sockaddr* a) {
    if (!a) return true;
    if (a->sa_family == AF_INET) { uint32_t ip = ntohl(((const sockaddr_in*)a)->sin_addr.s_addr); return (ip >> 24) == 127 || ip == 0; }
    if (a->sa_family == AF_INET6) { const in6_addr& i6 = ((const sockaddr_in6*)a)->sin6_addr; static const in6_addr lo = IN6ADDR_LOOPBACK_INIT; return memcmp(&i6, &lo, sizeof lo) == 0; }
    return true;
}
static int WSAAPI hkConnect(SOCKET s, const sockaddr* a, int len) {
    if (!g_set.blockNetwork || IsLocal(a)) { g_netAllowed++; return oConnect(s, a, len); }
    char ip[64] = "?"; int port = 0;
    if (a->sa_family == AF_INET) { inet_ntop(AF_INET, &((const sockaddr_in*)a)->sin_addr, ip, sizeof ip); port = ntohs(((const sockaddr_in*)a)->sin_port); }
    else if (a->sa_family == AF_INET6) { inet_ntop(AF_INET6, &((const sockaddr_in6*)a)->sin6_addr, ip, sizeof ip); port = ntohs(((const sockaddr_in6*)a)->sin6_port); }
    if (g_netBlocked++ < 30) Log("NET blocked a connection to %s port %d (offline mod)", ip, port);
    WSASetLastError(WSAECONNREFUSED);
    return SOCKET_ERROR;
}
typedef INT(WSAAPI* getaddrinfo_t)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
static getaddrinfo_t oGetAddrInfo;
static INT WSAAPI hkGetAddrInfo(PCSTR node, PCSTR svc, const ADDRINFOA* h, PADDRINFOA* r) {
    if (node && g_set.blockNetwork && _stricmp(node, "localhost") && strncmp(node, "127.", 4)) { static int n = 0; if (n++ < 30) Log("NET name lookup %s refused (offline mod)", node); return WSAHOST_NOT_FOUND; }
    return oGetAddrInfo(node, svc, h, r);
}
typedef INT(WSAAPI* GetAddrInfoW_t)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
static GetAddrInfoW_t oGetAddrInfoW;
static INT WSAAPI hkGetAddrInfoW(PCWSTR node, PCWSTR svc, const ADDRINFOW* h, PADDRINFOW* r) {
    if (node && g_set.blockNetwork && _wcsicmp(node, L"localhost") && wcsncmp(node, L"127.", 4)) { static int n = 0; if (n++ < 30) Log("NET name lookup %ls refused (offline mod)", node); return WSAHOST_NOT_FOUND; }
    return oGetAddrInfoW(node, svc, h, r);
}
static void HookNetwork() {
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    struct { const char* name; void* detour; void** orig; } h[] = {
        {"connect", (void*)&hkConnect, (void**)&oConnect}, {"getaddrinfo", (void*)&hkGetAddrInfo, (void**)&oGetAddrInfo}, {"GetAddrInfoW", (void*)&hkGetAddrInfoW, (void**)&oGetAddrInfoW}};
    for (auto& x : h) {
        void* p = ws ? (void*)GetProcAddress(ws, x.name) : nullptr;
        bool ok = p && MH_CreateHook(p, x.detour, x.orig) == MH_OK && MH_EnableHook(p) == MH_OK;
        Log("HOOK ws2_32 %s %s", x.name, ok ? "in place" : "FAILED");
    }
    Log("NET the game's internet connections are %s (AILimitVR.ini [Network] Block)", g_set.blockNetwork ? "refused and logged" : "allowed");
}

// ---------------------------------------------------------------- setup
typedef void* (*RuntimeInvoke_t)(MethodInfo*, void*, void**, void**);
static RuntimeInvoke_t oRuntimeInvoke;
static bool g_setupDone = false;
static double g_setupT0;
static void Setup();
static void* hkRuntimeInvoke(MethodInfo* m, void* obj, void** params, void** exc) {
    if (!g_setupDone) { g_setupDone = true; if (FindClass("UnityEngine.Rendering.HighDefinition", "HDRenderPipeline")) Setup(); else g_setupDone = false; }
    return oRuntimeInvoke(m, obj, params, exc);
}
static void Hook(MethodInfo* m, void* detour, void** orig, const char* name) {
    if (!m) { Log("HOOK %s NOT FOUND", name); return; }
    MH_STATUS a = MH_CreateHook(m->methodPointer, detour, orig), b = MH_EnableHook(m->methodPointer);
    Log("HOOK %s %s", name, (a == MH_OK && b == MH_OK) ? "in place" : "FAILED");
}

static void Setup() {
    Log("IL2CPP ready, setup on the main thread after %.2f s", Now() - g_setupT0);
    kCamera = FindClass("UnityEngine", "Camera");
    kHDCam = FindClass("UnityEngine.Rendering.HighDefinition", "HDAdditionalCameraData");
    kCanvas = FindClass("UnityEngine", "Canvas");
    kVolume = FindClass("UnityEngine.Rendering", "Volume");
    kGameObject = FindClass("UnityEngine", "GameObject");
    mCamGetTarget = Need(kCamera, "Camera", "get_targetTexture", 0); mCamSetTarget = Need(kCamera, "Camera", "set_targetTexture", 1);
    mCamCopyFrom = Need(kCamera, "Camera", "CopyFrom", 1); mCamSetProj = Need(kCamera, "Camera", "set_projectionMatrix", 1);
    mCamGetNear = Need(kCamera, "Camera", "get_nearClipPlane", 0); mCamGetFar = Need(kCamera, "Camera", "get_farClipPlane", 0);
    mCamSetFov = Need(kCamera, "Camera", "set_fieldOfView", 1); mCamSetAspect = Need(kCamera, "Camera", "set_aspect", 1);
    mCamGetDepth = Need(kCamera, "Camera", "get_depth", 0); mCamSetDepth = Need(kCamera, "Camera", "set_depth", 1);
    mCamSetMask = Need(kCamera, "Camera", "set_cullingMask", 1); mCamSetClearFlags = Need(kCamera, "Camera", "set_clearFlags", 1);
    mCamSetBg = Need(kCamera, "Camera", "set_backgroundColor", 1);
    mCamSetOrtho = Need(kCamera, "Camera", "set_orthographic", 1); mCamSetOrthoSize = Need(kCamera, "Camera", "set_orthographicSize", 1);
    mCamSetNear = Need(kCamera, "Camera", "set_nearClipPlane", 1); mCamSetFar = Need(kCamera, "Camera", "set_farClipPlane", 1);
    void* kComp = FindClass("UnityEngine", "Component");
    mCompTransform = Need(kComp, "Component", "get_transform", 0); mCompGet = Need(kComp, "Component", "GetComponent", 1, "System.Type");
    void* kTr = FindClass("UnityEngine", "Transform");
    mTrPos = Need(kTr, "Transform", "get_position", 0); mTrRot = Need(kTr, "Transform", "get_rotation", 0);
    mTrSetPosRot = Need(kTr, "Transform", "SetPositionAndRotation", 2);
    mGoCtor = Need(kGameObject, "GameObject", ".ctor", 1, "System.String"); mGoAdd = Need(kGameObject, "GameObject", "AddComponent", 1, "System.Type");
    void* kObj = FindClass("UnityEngine", "Object");
    mObjDontDestroy = Need(kObj, "Object", "DontDestroyOnLoad", 1); mObjFindAll = Need(kObj, "Object", "FindObjectsOfType", 1, "System.Type");
    mHDCopyTo = Need(kHDCam, "HDAdditionalCameraData", "CopyTo", 1);
    oHdClearMode = NeedField(kHDCam, "HDAdditionalCameraData", "clearColorMode"); oHdBg = NeedField(kHDCam, "HDAdditionalCameraData", "backgroundColorHDR");
    oHdVolMask = NeedField(kHDCam, "HDAdditionalCameraData", "volumeLayerMask"); oHdAA = NeedField(kHDCam, "HDAdditionalCameraData", "antialiasing");
    void* kRT = FindClass("UnityEngine", "RenderTexture");
    mRTCtor = Need(kRT, "RenderTexture", ".ctor", 5, "System.Int32,System.Int32,System.Int32,UnityEngine.RenderTextureFormat,UnityEngine.RenderTextureReadWrite");
    mRTCreate = Need(kRT, "RenderTexture", "Create", 0);
    mTexNative = FindMethod(FindClass("UnityEngine", "Texture"), "GetNativeTexturePtr", 0);
    g_texNativeIcall = (TexNativeIcall_t)il2cpp_resolve_icall("UnityEngine.Texture::GetNativeTexturePtr");
    Log("HOOKMAP Texture.GetNativeTexturePtr method %s, icall %s", mTexNative ? "found" : "stripped", g_texNativeIcall ? "found" : "NOT FOUND");
    if (!mTexNative && !g_texNativeIcall) g_missing++;
    mCanvasMode = Need(kCanvas, "Canvas", "get_renderMode", 0); mCanvasSetMode = Need(kCanvas, "Canvas", "set_renderMode", 1);
    mCanvasSetCam = Need(kCanvas, "Canvas", "set_worldCamera", 1); mCanvasSetPlane = Need(kCanvas, "Canvas", "set_planeDistance", 1);
    mCanvasRoot = Need(kCanvas, "Canvas", "get_isRootCanvas", 0);
    oVolProfile = NeedField(kVolume, "Volume", "sharedProfile");
    mObjName = Need(kObj, "Object", "get_name", 0); mCompGO = Need(kComp, "Component", "get_gameObject", 0);
    mGoLayer = Need(kGameObject, "GameObject", "get_layer", 0); mGoActive = Need(kGameObject, "GameObject", "get_activeInHierarchy", 0);
    mCanvasGetCam = Need(kCanvas, "Canvas", "get_worldCamera", 0); mCanvasOrder = Need(kCanvas, "Canvas", "get_sortingOrder", 0);
    kCPV = FindClass("UnityEngine.Rendering.HighDefinition", "CustomPassVolume");
    mObjInstantiate = Need(kObj, "Object", "Instantiate", 1, "UnityEngine.Object");
    mGoInParent = Need(kGameObject, "GameObject", "GetComponentInParent", 1, "System.Type");
    mTrSetPos = Need(kTr, "Transform", "set_position", 1); mCamSetAllowDyn = Need(kCamera, "Camera", "set_allowDynamicResolution", 1);
    oHdAllowDyn = NeedField(kHDCam, "HDAdditionalCameraData", "allowDynamicResolution"); oHdAllowDlss = NeedField(kHDCam, "HDAdditionalCameraData", "allowDeepLearningSuperSampling");
    oHdDlssCustomQ = NeedField(kHDCam, "HDAdditionalCameraData", "deepLearningSuperSamplingUseCustomQualitySettings"); oHdDlssQ = NeedField(kHDCam, "HDAdditionalCameraData", "deepLearningSuperSamplingQuality");
    oHdDlssCustomAttr = NeedField(kHDCam, "HDAdditionalCameraData", "deepLearningSuperSamplingUseCustomAttributes"); oHdDlssOptimal = NeedField(kHDCam, "HDAdditionalCameraData", "deepLearningSuperSamplingUseOptimalSettings");
    void* kAsset = FindClass("UnityEngine.Rendering.HighDefinition", "HDRenderPipelineAsset");
    oAssetSettings = NeedField(kAsset, "HDRenderPipelineAsset", "m_RenderPipelineSettings");
    void* kRPS = FindClass("UnityEngine.Rendering.HighDefinition", "RenderPipelineSettings");
    int d = NeedField(kRPS, "RenderPipelineSettings", "dynamicResolutionSettings"); oDynRes = d >= 0x10 ? d - 0x10 : d;  // value-type field offsets count the object header
    void* kDyn = FindClass("UnityEngine.Rendering", "GlobalDynamicResolutionSettings");
    int a = NeedField(kDyn, "GlobalDynamicResolutionSettings", "enabled"), b = NeedField(kDyn, "GlobalDynamicResolutionSettings", "enableDLSS");
    oDynEnabled = a >= 0x10 ? a - 0x10 : a; oDynDlss = b >= 0x10 ? b - 0x10 : b;
    Log("HOOKMAP DLSS settings at asset+0x%X+0x%X (+%d enabled, +%d DLSS); the dump says dynamicResolutionSettings 0x15C, enabled 0, enableDLSS 2", oAssetSettings, oDynRes, oDynEnabled, oDynDlss);
    void* kDrh = FindClass("UnityEngine.Rendering", "DynamicResolutionHandler");
    mDrhInstance = Need(kDrh, "DynamicResolutionHandler", "get_instance", 0); mDrhScale = Need(kDrh, "DynamicResolutionHandler", "GetCurrentScale", 0);
    kGlobal = FindClass("", "Global");
    oGlTargetRoot = NeedField(kGlobal, "Global", "TargetPointUIRoot"); oGlTargetPoint = NeedField(kGlobal, "Global", "_TargetPoint");
    oHideRoot = NeedField(FindClass("", "HideUIElement"), "HideUIElement", "Root");
    oCpvGlobal = NeedField(kCPV, "CustomPassVolume", "m_IsGlobal"); oCpvPasses = NeedField(kCPV, "CustomPassVolume", "customPasses");
    oCpvInject = NeedField(kCPV, "CustomPassVolume", "injectionPoint"); oCpvCam = NeedField(kCPV, "CustomPassVolume", "m_TargetCamera");
    oCpvUseCam = NeedField(kCPV, "CustomPassVolume", "useTargetCamera");
    void* kPass = FindClass("UnityEngine.Rendering.HighDefinition", "CustomPass");
    oPassName = NeedField(kPass, "CustomPass", "m_Name"); oPassEnabled = NeedField(kPass, "CustomPass", "enabled");
    void* kVM = FindClass("UnityEngine.Rendering", "VolumeManager");
    mVolMgrInstance = Need(kVM, "VolumeManager", "get_instance", 0); oVolMgrVolumes = NeedField(kVM, "VolumeManager", "m_Volumes");
    void* kProf = FindClass("UnityEngine.Rendering", "VolumeProfile");
    oProfComps = NeedField(kProf, "VolumeProfile", "components");
    oCompActive = NeedField(FindClass("UnityEngine.Rendering", "VolumeComponent"), "VolumeComponent", "active");
    void* kTime = FindClass("UnityEngine", "Time");
    mTimeTime = Need(kTime, "Time", "get_time", 0); mTimeScale = Need(kTime, "Time", "get_timeScale", 0);
    mTimeFixedDelta = Need(kTime, "Time", "get_fixedDeltaTime", 0); mTimeMaxDelta = Need(kTime, "Time", "get_maximumDeltaTime", 0);
    mTimeFrame = Need(kTime, "Time", "get_frameCount", 0);
    void* kQS = FindClass("UnityEngine", "QualitySettings");
    mVSync = Need(kQS, "QualitySettings", "get_vSyncCount", 0); mSetVSync = Need(kQS, "QualitySettings", "set_vSyncCount", 1);
    void* kApp = FindClass("UnityEngine", "Application");
    mTargetFps = Need(kApp, "Application", "get_targetFrameRate", 0); mSetTargetFps = Need(kApp, "Application", "set_targetFrameRate", 1);
    void* kScreen = FindClass("UnityEngine", "Screen");
    mScreenW = Need(kScreen, "Screen", "get_width", 0); mScreenH = Need(kScreen, "Screen", "get_height", 0);
    mPrefsGetInt = Need(FindClass("UnityEngine", "PlayerPrefs"), "PlayerPrefs", "GetInt", 2);
    void* kIS = FindClass("UnityEngine.InputSystem", "InputSystem");
    mListEnabledActions = Need(kIS, "InputSystem", "ListEnabledActions", 0);
    void* kAct = FindClass("UnityEngine.InputSystem", "InputAction");
    mActionEnable = Need(kAct, "InputAction", "Enable", 0); mActionDisable = Need(kAct, "InputAction", "Disable", 0);

    g_issue = (IssuePluginEvent_t)il2cpp_resolve_icall("UnityEngine.GL::GLIssuePluginEvent");
    Log("HOOKMAP icall GL::GLIssuePluginEvent %s", g_issue ? "found" : "NOT FOUND");
    void* kPipe = FindClass("UnityEngine.Rendering.HighDefinition", "HDRenderPipeline");
    mRender = Need(kPipe, "HDRenderPipeline", "Render", 2, "UnityEngine.Rendering.ScriptableRenderContext,System.Collections.Generic.List<UnityEngine.Camera>");
    if (g_missing) Log("HOOKMAP %d item(s) not found: VR stays off", g_missing);
    if (g_missing || !g_issue || !mRender) { g_vrOn = false; return; }
    Hook(mRender, (void*)&hkRender, (void**)&oRender, "HDRenderPipeline.Render(List<Camera>)");
}

static DWORD WINAPI InitThread(void*) {
    g_setupT0 = Now();
    while (!(g_ga = GetModuleHandleW(L"GameAssembly.dll"))) Sleep(1);
    MH_Initialize();
    HookNetwork();
    if (!BindIl2cpp()) return 0;
    void* ri = GetProcAddress(g_ga, "il2cpp_runtime_invoke");
    MH_STATUS a = MH_CreateHook(ri, (void*)&hkRuntimeInvoke, (void**)&oRuntimeInvoke), b = MH_EnableHook(ri);
    Log("HOOK il2cpp_runtime_invoke (setup on the main thread) %s, GameAssembly seen after %.3f s", (a == MH_OK && b == MH_OK) ? "in place" : "FAILED", Now() - g_setupT0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_qp0);
        timeBeginPeriod(1);
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        g_gameDir = p; g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_gameDir + L"AILimitVR.log").c_str(), L"w");
        LoadSettings(); SaveSettings();
        Log("AILimitVR %s (build %d), built " __DATE__ " " __TIME__, MOD_VERSION, MOD_BUILD);
        Log("SETTINGS resolution %.2f, picture %d, HUD mode %d size %.2f x %.2f y %.2f distance %.2f, menu size %.2f height %.2f distance %.2f, block network %d",
            g_set.resolution, g_set.picture, g_set.hudMode, g_set.hudWidth, g_set.hudX, g_set.hudY, g_set.hudDistance, g_set.menuSize, g_set.menuHeight, g_set.menuDistance, g_set.blockNetwork);
        aixr::SetLogger(&Log);
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
