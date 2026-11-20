// OpenXR side of AI Limit VR (64-bit, D3D11). Ported from Ori WotW VR's WotwXR (same author, MIT).
// The game never waits for the headset: OpenXR runs on its own thread with its own D3D11 device. The game's render
// thread copies each finished eye pair (and the HUD picture) into a ring of shared textures (keyed mutex, never waited
// on: a busy slot is skipped and counted), and the XR thread submits the newest pair as the projection layer, with the
// head pose each eye was rendered from. The HUD and the mod menu are flat panels: quad layers, or drawn into the
// projection images on Virtual Desktop (Picture Auto, TECHNIQUES "Flat panels: quad or projection layer").
#pragma once
#include <cstdint>

namespace aixr {

enum State { Off = 0, Starting = 1, Running = 2, Failed = 3, Stopped = 4 };

struct Views {
    bool valid = false;        // eye poses usable
    bool focused = false;      // session state FOCUSED
    int sessionState = 0;      // XrSessionState
    double periodSec = 0;      // the runtime's display period
    float local[2][3] = {};    // each eye relative to the recenter point, in the recenter frame (metres; OpenXR: x right, y up, z back)
    float localRot[2][4] = {}; // each eye's orientation relative to the recenter facing (xyzw, OpenXR)
    float pose[2][7] = {};     // each eye in the room: position xyz, orientation xyzw (as located)
    float fov[2][4] = {};      // angles left, right, up, down (radians)
    int64_t displayTime = 0;
    uint32_t recenters = 0;    // counts recenters, so the game side can tell a new one happened
};

// Flat panel placement. Fixed: placed in the room at recenter. Follows head: held in front of the head.
struct Panel { int mode = 0 /*0 Fixed, 1 Follows head, 2 Hidden*/; float width = 1.6f, x = 0, y = -0.05f, distance = 1.6f; };

void SetLogger(void (*log)(const char* fmt, ...));
// Eye image size = the headset's recommended size times the resolution setting (read once, at start).
void Configure(int picture /*0 Auto, 1 Steady (projection), 2 Panel (quad)*/, float resolution, int hudW, int hudH);
int EyeWidth();                     // 0 until the ring is ready
int EyeHeight();
void SetHud(const Panel& p);        // live
void Start();                       // starts the XR thread once
int GetState();
const char* RuntimeName();
const char* HeadsetName();
int RecommendedWidth();
int RecommendedHeight();
int LayerInUse();                   // 1 projection (panels drawn into the eyes), 0 quad panels, -1 none yet
int LastError();
bool GetViews(Views* out);
void Recenter();
// Game main thread: hand over this frame's eye textures and HUD texture (Unity native pointers) and the views they
// were rendered with, then call GL.IssuePluginEvent(RenderEventFunc(), 1). The copy happens on the game's render thread.
void QueueFrame(void* texL, void* texR, void* hud, const Views& used, uint32_t frameNo);
// GL.IssuePluginEvent(RenderEventFunc(), 3) at the start of the frame's rendering: GPU timestamp for the GPU time line.
void* RenderEventFunc();
void Shutdown();

struct Stats {
    uint32_t queued, copied, skippedBusy, skippedNoSlots, xrFrames, xrNoNewImage, layersSent, gpuSamples;
    double waitMsAvg, waitMsMax, endMsAvg, endMsMax, copyMsMax, gpuMsAvg, gpuMsMax;
};
void TakeStats(Stats* out);         // returns counters since the last call and resets them

// ------------------------------------------------------------------------------------------------ mod menu panel
// The menu's content is built on the game's main thread (native\Menu.inc) and handed over as a snapshot; the XR thread
// draws it with Dear ImGui into its own texture and shows it in front of the player.
struct MenuRow { char label[48]; char value[48]; bool button; };
struct MenuSnapshot {
    bool visible = false;
    int tabCount = 0, tab = 0;
    char tabs[6][24] = {};
    int rowCount = 0, row = 0;
    MenuRow rows[12] = {};
    char explain[400] = "";
    char footer[120] = "";
    char version[48] = "";
    char banner[160] = "";                                 // VR DID NOT START and similar
    float size = 1.0f, height = 0.0f, distance = 0.9f;   // placement: scale, metres up from eye height, metres away
};
void SetMenu(const MenuSnapshot& m);

}  // namespace aixr
