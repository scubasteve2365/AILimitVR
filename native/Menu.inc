// Mod menu (Phase 1 rows only), included by AILimitVRNative.cpp. Runs on the game's main thread; the XR thread draws
// the snapshot (xr\MenuDraw.cpp). Ported from Ori WotW VR's Menu.inc (same author, MIT).
// Opens with both sticks held 0.5 s on an Xbox pad, or Insert (not in the game's key
// bindings). While it is open the game's input actions are switched off, so the game gets no pad or keyboard input;
// after closing they come back once every key and button is released. The menu reads the pad itself through XInput.

namespace menu {

enum Kind { Button, Value };
struct Row { const char* label; Kind kind; const char* explain; };
struct Tab { const char* name; std::vector<Row> rows; };
static std::vector<Tab> g_tabs;
static bool g_open = false, g_block = false;
static int g_tab = 0, g_row = 0;
static double g_bothSticksSince = -1;
static bool g_comboUsed = false;

enum { PadUp = 0x0001, PadDown = 0x0002, PadLeft = 0x0004, PadRight = 0x0008, PadStart = 0x0010, PadBack = 0x0020,
       PadL3 = 0x0040, PadR3 = 0x0080, PadLB = 0x0100, PadRB = 0x0200, PadA = 0x1000, PadB = 0x2000 };
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XState*);
static XInputGetState_t g_xget;
static int g_padSlot = -1;
static double g_padScan = 0;
static uint32_t g_padReads = 0;

static void Build() {
    g_tabs = {
        {"GENERAL", {
            {"Recenter", Button, "Puts the view, the HUD and this menu in front of you again, at your current height and facing. The headset's own recenter does the same."},
            {"World scale", Value, "How big the world feels. 1.00 is life size. Higher makes the world smaller with more 3D depth; lower makes it bigger and flatter. Applies at once."},
            {"Upscaler", Value, nullptr},
            {"Decoupled pitch", Value, "On: the world stays level. Tilting the camera up or down with the stick (or lock-on) no longer tilts your view; look up and down with your head. Off: the view tilts with the game's camera, as in the flat game. Applies at once."},
            {"Detail distance", Value, "How far away the game keeps full detail and small objects. Full matches the flat game. The headset's wide view makes things look smaller to the game, so lower settings drop ground, props and detail sooner. Medium and Short save some CPU time. Applies at once."},
            {"Resolution", Value, nullptr},
            {"Picture", Value, nullptr},
            {"Close menu", Button, "Closes the mod menu. Insert or both sticks held also close it."}}},
        {"HUD", {
            {"HUD mode", Value, nullptr},
            {"HUD size", Value, "Width of the HUD panel in metres. The game's full-screen menus show on the same panel."},
            {"HUD left / right", Value, "Moves the HUD panel left or right, in metres."},
            {"HUD height", Value, "Moves the HUD panel up or down from eye height, in metres."},
            {"HUD distance", Value, "How far the HUD panel stands in front of you, in metres."},
            {"Lock-on marker height", Value, "Raises or lowers the lock-on marker on the HUD panel, in screen pixels of the game's picture."}}},
        {"MOD MENU", {
            {"Menu size", Value, "Size of this menu. It has its own place, separate from the HUD."},
            {"Menu height", Value, "Moves this menu up or down from eye height, in metres."},
            {"Menu distance", Value, "How far this menu stands in front of you, in metres."},
            {"Menu sounds", Value, "Soft clicks when you move, change, switch tabs, open or close this menu. Off, Low, Medium or High."}}},
        {"GAME", {
            {"Load VR baseline settings", Button, nullptr},
            {"Load best VR performance", Button, nullptr}}},
    };
}

static int NextW() { return (((int)lroundf(aixr::RecommendedWidth() * g_set.resolution)) + 1) & ~1; }
static int NextH() { return (((int)lroundf(aixr::RecommendedHeight() * g_set.resolution)) + 1) & ~1; }

static void ValueText(const char* label, char* out, size_t n) {
    std::string l = label;
    if (l == "Resolution") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.resolution * 100));
    else if (l == "HUD mode") sprintf_s(out, n, "%s", g_set.hudMode == 0 ? "Fixed" : g_set.hudMode == 1 ? "Follows head" : "Hidden");
    else if (l == "HUD size") sprintf_s(out, n, "%.2f m", g_set.hudWidth);
    else if (l == "HUD left / right") sprintf_s(out, n, "%+.2f m", g_set.hudX);
    else if (l == "HUD height") sprintf_s(out, n, "%+.2f m", g_set.hudY);
    else if (l == "HUD distance") sprintf_s(out, n, "%.2f m", g_set.hudDistance);
    else if (l == "World scale") sprintf_s(out, n, "%.2f", g_set.worldScale);
    else if (l == "Decoupled pitch") sprintf_s(out, n, "%s", g_set.decoupledPitch ? "On" : "Off");
    else if (l == "Detail distance") sprintf_s(out, n, "%s", g_set.lodMatch == 0 ? "Short" : g_set.lodMatch == 2 ? "Medium" : "Full");
    else if (l == "Upscaler") { static const char* u[] = {"TAA", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Perf"}; sprintf_s(out, n, "%s", u[g_set.upscaler % 6]); }
    else if (l == "Lock-on marker height") sprintf_s(out, n, "%+d px", (int)g_set.lockOnLift);
    else if (l == "Menu size") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.menuSize * 100));
    else if (l == "Menu height") sprintf_s(out, n, "%+.2f m", g_set.menuHeight);
    else if (l == "Menu distance") sprintf_s(out, n, "%.2f m", g_set.menuDistance);
    else if (l == "Picture") { int li = aixr::LayerInUse(); sprintf_s(out, n, "%s%s", g_set.picture == 1 ? "Steady" : g_set.picture == 2 ? "Panel" : "Auto", g_set.picture == 0 ? (li == 1 ? " (Steady)" : li == 0 ? " (Panel)" : "") : ""); }
    else if (l == "Menu sounds") { static const char* v[] = {"Off", "Low", "Medium", "High"}; sprintf_s(out, n, "%s", v[g_set.menuSound & 3]); }
    else out[0] = 0;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void PushHud() { aixr::Panel p; p.mode = g_set.hudMode; p.width = g_set.hudWidth; p.x = g_set.hudX; p.y = g_set.hudY; p.distance = g_set.hudDistance; aixr::SetHud(p); }
static void ChangeValue(const char* label, int dir);
static void Change(const char* label, int dir) {
    char before[48], after[48]; ValueText(label, before, sizeof before);
    ChangeValue(label, dir);
    ValueText(label, after, sizeof after);
    PlayNav(strcmp(before, after) ? SndChange : SndRefused);
}
static void ChangeValue(const char* label, int dir) {
    std::string l = label;
    if (l == "Resolution") g_set.resolution = Clamp(roundf((g_set.resolution + dir * 0.05f) * 20) / 20, 0.5f, 2.0f);
    else if (l == "HUD mode") g_set.hudMode = (g_set.hudMode + 3 + dir) % 3;
    else if (l == "HUD size") g_set.hudWidth = Clamp(g_set.hudWidth + dir * 0.05f, 0.3f, 4.0f);
    else if (l == "HUD left / right") g_set.hudX = Clamp(g_set.hudX + dir * 0.02f, -1.5f, 1.5f);
    else if (l == "HUD height") g_set.hudY = Clamp(g_set.hudY + dir * 0.02f, -1.5f, 1.5f);
    else if (l == "HUD distance") g_set.hudDistance = Clamp(g_set.hudDistance + dir * 0.05f, 0.4f, 5.0f);
    else if (l == "World scale") { float st = g_set.worldScale + dir * 0.01f >= 2.0f ? 0.25f : 0.05f; g_set.worldScale = Clamp(roundf((g_set.worldScale + dir * st) * 100) / 100, 0.25f, 10.0f); }
    else if (l == "Decoupled pitch") g_set.decoupledPitch = !g_set.decoupledPitch;
    else if (l == "Detail distance") { static const int order[3] = {0, 2, 1}; int i = g_set.lodMatch == 0 ? 0 : g_set.lodMatch == 2 ? 1 : 2; i = (int)Clamp((float)(i + dir), 0, 2); g_set.lodMatch = order[i]; g_lodApplied = -1; }
    else if (l == "Upscaler") { g_set.upscaler = (g_set.upscaler + 6 + dir) % 6; ApplyUpscaler("menu"); }
    else if (l == "Lock-on marker height") g_set.lockOnLift = Clamp(g_set.lockOnLift + dir * 5, -300, 300);
    else if (l == "Menu size") g_set.menuSize = Clamp(roundf((g_set.menuSize + dir * 0.05f) * 20) / 20, 0.5f, 2.0f);
    else if (l == "Menu height") g_set.menuHeight = Clamp(g_set.menuHeight + dir * 0.02f, -0.6f, 0.6f);
    else if (l == "Menu distance") g_set.menuDistance = Clamp(g_set.menuDistance + dir * 0.05f, 0.3f, 2.0f);
    else if (l == "Picture") { g_set.picture = (g_set.picture + 3 + dir) % 3; aixr::SetPicture(g_set.picture); }
    else if (l == "Menu sounds") g_set.menuSound = (int)Clamp((float)(g_set.menuSound + dir), 0, 3);
    else return;
    if (l.rfind("HUD", 0) == 0) PushHud();
    SaveSettings();
    char v[48]; ValueText(label, v, sizeof v);
    Log("MENU %s -> %s (saved)", label, v);
}

// The two settings buttons change many game settings: the first press arms, a second press within 5 s confirms.
static std::string g_armed; static double g_armedAt = 0;
static void Press(const char* label) {
    std::string l = label;
    Log("MENU pressed %s", label);
    PlayNav(SndChange);
    if (l == "Recenter") aixr::Recenter();
    else if (l == "Close menu") { g_open = false; Log("MENU closed"); }
    else if (l == "Load VR baseline settings" || l == "Load best VR performance") {
        if (g_armed != l || Now() - g_armedAt > 5) { g_armed = l; g_armedAt = Now(); Log("MENU %s armed: press again to confirm", label); return; }
        g_armed.clear();
        if (l == "Load VR baseline settings") LoadVrBaseline(); else LoadBestPerformance();
    }
}

static void Snapshot() {
    aixr::MenuSnapshot s;
    s.visible = g_open;
    s.tabCount = (int)g_tabs.size(); s.tab = g_tab;
    for (int i = 0; i < s.tabCount; i++) strcpy_s(s.tabs[i], g_tabs[i].name);
    const Tab& t = g_tabs[g_tab];
    s.rowCount = (int)t.rows.size(); s.row = g_row;
    for (int i = 0; i < s.rowCount; i++) {
        strcpy_s(s.rows[i].label, t.rows[i].label);
        ValueText(t.rows[i].label, s.rows[i].value, sizeof s.rows[i].value);
        s.rows[i].button = t.rows[i].kind == Button;
    }
    const Row& r = t.rows[g_row];
    std::string l = r.label;
    if (r.explain) strcpy_s(s.explain, r.explain);
    else if (l == "Resolution")
        sprintf_s(s.explain, "Sharpness of the 3D picture. 100%% is the size your headset asks for. Running now: %dx%d per eye. Next launch: %dx%d. Takes effect the next time you start the game.",
                  g_eyeW, g_eyeH, NextW(), NextH());
    else if (l == "Upscaler")
        sprintf_s(s.explain, "How each eye is drawn. TAA: the game's own smoothing at full size. DLAA: NVIDIA's at full size, sharper. DLSS Quality to Ultra Performance: each eye drawn smaller and rebuilt by NVIDIA DLSS for more frame rate (RTX cards). Applies at once. Now: %s.", DlssStatus());
    else if (l == "Picture")
        strcpy_s(s.explain, "How the HUD and this menu are shown. Auto (recommended) uses Steady on Virtual Desktop (Quest) and Panel on SteamVR (Pimax, Index, Vive and others). Steady draws them into the 3D picture: rock steady on Virtual Desktop. Panel shows them as separate sharp panels: best on SteamVR, but Virtual Desktop squashes them when you turn your head.");
    else if (l == "Load VR baseline settings")
        strcpy_s(s.explain, (g_armed == l && Now() - g_armedAt < 5) ? "Press A again to confirm. Sets the game's graphics back to its own defaults with VSync, LOCK FPS, motion blur and ambient occlusion off." : "Sets the game's own graphics settings back to what VR needs: its default quality, with VSync, LOCK FPS, motion blur and ambient occlusion off. Your mod settings are not touched. Press twice to confirm.");
    else if (l == "Load best VR performance")
        strcpy_s(s.explain, (g_armed == l && Now() - g_armedAt < 5) ? "Press A again to confirm. Lowest shadows and fog, ambient occlusion and motion blur off, DLSS on." : "Sets the game for the highest VR frame rate: lowest shadows and fog, ambient occlusion and motion blur off, VSync and LOCK FPS off, and DLSS on in both eyes if it was off. Textures are kept. Press twice to confirm.");
    else if (l == "HUD mode")
        strcpy_s(s.explain, g_set.hudMode == 0 ? "Fixed: the HUD stays in place in the room, in front of where you recentered. Look at it when you need it."
                          : g_set.hudMode == 1 ? "Follows head: the HUD stays in front of your eyes wherever you look."
                                               : "Hidden: no HUD panel. The game's full-screen menus are hidden too while this is set.");
    strcpy_s(s.footer, "A Select    B Close    LB / RB Tabs");
    sprintf_s(s.version, "%s", MOD_VERSION);
    if (aixr::GetState() == aixr::Failed)
        sprintf_s(s.banner, "VR DID NOT START (OpenXR error %d). Start SteamVR or Virtual Desktop first, then restart the game. The log AILimitVR.log in the game folder has the details.", aixr::LastError());
    s.size = g_set.menuSize; s.height = g_set.menuHeight; s.distance = g_set.menuDistance;
    aixr::SetMenu(s);
}

// Repeat for held directions: first step at once, then after 0.4 s every 0.12 s.
struct Repeat { double since = -1, last = 0; bool Step(bool held, double now) {
    if (!held) { since = -1; return false; }
    if (since < 0) { since = now; last = now; return true; }
    if (now - since > 0.4 && now - last > 0.12) { last = now; return true; }
    return false; } };
static Repeat g_rUp, g_rDown, g_rLeft, g_rRight;
static bool g_prevA, g_prevB, g_prevLB, g_prevRB, g_prevIns;

static bool OurWindowFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The pad: one connected slot is read each frame; empty slots are only searched every 2 s (reading an empty slot is slow).
static bool ReadPad(double now, XState& st) {
    if (!g_xget) {
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        g_xget = x ? (XInputGetState_t)GetProcAddress(x, "XInputGetState") : nullptr;
        Log("INPUT XInput %s", g_xget ? "loaded (xinput1_4.dll): the menu reads the pad itself" : "NOT loaded: the menu works with the keyboard only");
        if (!g_xget) g_xget = (XInputGetState_t)1;
    }
    if (g_xget == (XInputGetState_t)1) return false;
    if (g_padSlot >= 0) {
        g_padReads++;
        if (g_xget(g_padSlot, &st) == ERROR_SUCCESS) return true;
        Log("INPUT pad %d gone", g_padSlot); g_padSlot = -1;
    }
    if (now < g_padScan) return false;
    g_padScan = now + 2.0;
    for (int i = 0; i < 4; i++) if (g_xget(i, &st) == ERROR_SUCCESS) { g_padSlot = i; Log("INPUT Xbox pad found in slot %d", i); return true; }
    return false;
}

static void Update(double now) {
    if (g_tabs.empty()) Build();
    bool kb = OurWindowFocused();
    XState st{}; bool pad = ReadPad(now, st);
    WORD b = pad ? st.buttons : 0;
    SHORT ly = pad ? st.ly : 0, lx = pad ? st.lx : 0;
    bool ins = kb && Key(VK_INSERT);
    // Open or close: both sticks held 0.5 s, or Insert (one press, one toggle).
    bool both = (b & PadL3) && (b & PadR3);
    bool was = g_open;
    if (both) { if (g_bothSticksSince < 0) g_bothSticksSince = now; if (!g_comboUsed && now - g_bothSticksSince > 0.5) { g_comboUsed = true; g_open = !g_open; Log("MENU %s (both sticks)", g_open ? "opened" : "closed"); } }
    else { g_bothSticksSince = -1; g_comboUsed = false; }
    if (ins && !g_prevIns) { g_open = !g_open; Log("MENU %s (Insert)", g_open ? "opened" : "closed"); }
    g_prevIns = ins;
    bool anyHeld = b != 0 || abs(ly) > 8000 || abs(lx) > 8000 || (pad && (st.lt > 30 || st.rt > 30)) ||
                   (kb && (Key(VK_UP) || Key(VK_DOWN) || Key(VK_LEFT) || Key(VK_RIGHT) || Key(VK_RETURN) || Key(VK_ESCAPE) || Key(VK_BACK) || Key(VK_PRIOR) || Key(VK_NEXT) || ins));
    if (g_open != was) PlayNav(g_open ? SndOpen : SndClose);
    if (g_open && !g_block) { g_block = true; BlockGameInput(true); }
    else if (!g_open && g_block && !anyHeld) { g_block = false; BlockGameInput(false); Log("MENU input back to the game"); }
    if (g_open) {
        const Tab& t = g_tabs[g_tab];
        bool up = (b & PadUp) || ly > 16000 || (kb && Key(VK_UP));
        bool down = (b & PadDown) || ly < -16000 || (kb && Key(VK_DOWN));
        bool left = (b & PadLeft) || lx < -16000 || (kb && Key(VK_LEFT));
        bool right = (b & PadRight) || lx > 16000 || (kb && Key(VK_RIGHT));
        bool a = (b & PadA) || (kb && Key(VK_RETURN));
        bool back = (b & PadB) || (kb && (Key(VK_ESCAPE) || Key(VK_BACK)));
        bool lb = (b & PadLB) || (pad && st.lt > 100) || (kb && Key(VK_PRIOR)), rb = (b & PadRB) || (pad && st.rt > 100) || (kb && Key(VK_NEXT));
        if (!was) { g_prevA = a; g_prevB = back; g_prevLB = lb; g_prevRB = rb; }  // the opening press does nothing in the menu
        if (g_rUp.Step(up, now)) { g_row = (g_row + (int)t.rows.size() - 1) % (int)t.rows.size(); PlayNav(SndMove); }
        if (g_rDown.Step(down, now)) { g_row = (g_row + 1) % (int)t.rows.size(); PlayNav(SndMove); }
        const Row& r = t.rows[g_row];
        if (r.kind == Value) { if (g_rLeft.Step(left, now)) Change(r.label, -1); if (g_rRight.Step(right, now)) Change(r.label, +1); }
        else { g_rLeft.Step(false, now); g_rRight.Step(false, now); }
        if (a && !g_prevA && r.kind == Button) Press(r.label);
        if (back && !g_prevB) { g_open = false; Log("MENU closed (back)"); }
        if (lb && !g_prevLB) { g_tab = (g_tab + (int)g_tabs.size() - 1) % (int)g_tabs.size(); g_row = 0; PlayNav(SndTab); }
        if (rb && !g_prevRB) { g_tab = (g_tab + 1) % (int)g_tabs.size(); g_row = 0; PlayNav(SndTab); }
        g_prevA = a; g_prevB = back; g_prevLB = lb; g_prevRB = rb;
    }
    Snapshot();
}

}  // namespace menu
