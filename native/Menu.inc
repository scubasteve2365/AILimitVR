// Mod menu (Phase 1 rows only), included by AILimitVRNative.cpp. Runs on the game's main thread; the XR thread draws
// the snapshot (xr\MenuDraw.cpp). Ported from Ori WotW VR's Menu.inc (same author, MIT).
// Opens with both sticks held 0.5 s on an Xbox pad, or Insert (not in the game's key bindings, RE_NOTES "Input
// bindings"). While it is open the game's input actions are switched off, so the game gets no pad or keyboard input;
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
            {"Resolution", Value, nullptr},
            {"Close menu", Button, "Closes the mod menu. Insert or both sticks held also close it."}}},
        {"HUD", {
            {"HUD mode", Value, nullptr},
            {"HUD size", Value, "Width of the HUD panel in metres. The game's full-screen menus show on the same panel."},
            {"HUD left / right", Value, "Moves the HUD panel left or right, in metres."},
            {"HUD height", Value, "Moves the HUD panel up or down from eye height, in metres."},
            {"HUD distance", Value, "How far the HUD panel stands in front of you, in metres."}}},
        {"MOD MENU", {
            {"Menu size", Value, "Size of this menu. It has its own place, separate from the HUD."},
            {"Menu height", Value, "Moves this menu up or down from eye height, in metres."},
            {"Menu distance", Value, "How far this menu stands in front of you, in metres."}}},
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
    else if (l == "Menu size") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.menuSize * 100));
    else if (l == "Menu height") sprintf_s(out, n, "%+.2f m", g_set.menuHeight);
    else if (l == "Menu distance") sprintf_s(out, n, "%.2f m", g_set.menuDistance);
    else out[0] = 0;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void PushHud() { aixr::Panel p; p.mode = g_set.hudMode; p.width = g_set.hudWidth; p.x = g_set.hudX; p.y = g_set.hudY; p.distance = g_set.hudDistance; aixr::SetHud(p); }
static void Change(const char* label, int dir) {
    std::string l = label;
    if (l == "Resolution") g_set.resolution = Clamp(roundf((g_set.resolution + dir * 0.05f) * 20) / 20, 0.5f, 2.0f);
    else if (l == "HUD mode") g_set.hudMode = (g_set.hudMode + 3 + dir) % 3;
    else if (l == "HUD size") g_set.hudWidth = Clamp(g_set.hudWidth + dir * 0.05f, 0.3f, 4.0f);
    else if (l == "HUD left / right") g_set.hudX = Clamp(g_set.hudX + dir * 0.02f, -1.5f, 1.5f);
    else if (l == "HUD height") g_set.hudY = Clamp(g_set.hudY + dir * 0.02f, -1.5f, 1.5f);
    else if (l == "HUD distance") g_set.hudDistance = Clamp(g_set.hudDistance + dir * 0.05f, 0.4f, 5.0f);
    else if (l == "Menu size") g_set.menuSize = Clamp(roundf((g_set.menuSize + dir * 0.05f) * 20) / 20, 0.5f, 2.0f);
    else if (l == "Menu height") g_set.menuHeight = Clamp(g_set.menuHeight + dir * 0.02f, -0.6f, 0.6f);
    else if (l == "Menu distance") g_set.menuDistance = Clamp(g_set.menuDistance + dir * 0.05f, 0.3f, 2.0f);
    else return;
    if (l.rfind("HUD", 0) == 0) PushHud();
    SaveSettings();
    char v[48]; ValueText(label, v, sizeof v);
    Log("MENU %s -> %s (saved)", label, v);
}

static void Press(const char* label) {
    std::string l = label;
    Log("MENU pressed %s", label);
    if (l == "Recenter") aixr::Recenter();
    else if (l == "Close menu") { g_open = false; Log("MENU closed"); }
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
    else if (l == "HUD mode")
        strcpy_s(s.explain, g_set.hudMode == 0 ? "Fixed: the HUD stays in place in the room, in front of where you recentered. Look at it when you need it."
                          : g_set.hudMode == 1 ? "Follows head: the HUD stays in front of your eyes wherever you look."
                                               : "Hidden: no HUD panel. The game's full-screen menus are hidden too while this is set.");
    strcpy_s(s.footer, "A Select    B Close    LB / RB Tabs");
    sprintf_s(s.version, "%s  build %d", MOD_VERSION, MOD_BUILD);
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
        if (g_rUp.Step(up, now)) g_row = (g_row + (int)t.rows.size() - 1) % (int)t.rows.size();
        if (g_rDown.Step(down, now)) g_row = (g_row + 1) % (int)t.rows.size();
        const Row& r = t.rows[g_row];
        if (r.kind == Value) { if (g_rLeft.Step(left, now)) Change(r.label, -1); if (g_rRight.Step(right, now)) Change(r.label, +1); }
        else { g_rLeft.Step(false, now); g_rRight.Step(false, now); }
        if (a && !g_prevA && r.kind == Button) Press(r.label);
        if (back && !g_prevB) { g_open = false; Log("MENU closed (back)"); }
        if (lb && !g_prevLB) { g_tab = (g_tab + (int)g_tabs.size() - 1) % (int)g_tabs.size(); g_row = 0; }
        if (rb && !g_prevRB) { g_tab = (g_tab + 1) % (int)g_tabs.size(); g_row = 0; }
        g_prevA = a; g_prevB = back; g_prevLB = lb; g_prevRB = rb;
    }
    Snapshot();
}

}  // namespace menu
