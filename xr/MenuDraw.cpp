// Draws the mod menu snapshot into a texture on the XR thread with Dear ImGui (ocornut/imgui, MIT), styled after AI
// LIMIT's own settings screens: near-black panel, a thin dark red rule under the title and above the footer, white serif
// text, the chosen row framed in a soft white outline, the explanation bottom left and button hints bottom right.
// Ported from Ori WotW VR's MenuDraw (same author, MIT). Font: Georgia from Windows' own font folder (not shipped).
#include "MenuDraw.h"
#include <windows.h>
#include <mutex>
#include <string>
#include "imgui.h"
#include "imgui_impl_dx11.h"

namespace aixr {

static std::mutex g_menuMx;
static MenuSnapshot g_menu;
void SetMenu(const MenuSnapshot& m) { std::lock_guard<std::mutex> lk(g_menuMx); g_menu = m; }

static const int kW = 1280, kH = 800;
static bool g_init = false, g_failed = false;
static ID3D11Texture2D* g_tex; static ID3D11RenderTargetView* g_rtv; static ID3D11ShaderResourceView* g_srv;
static ImFont *g_fTitle, *g_fText, *g_fSmall;

static bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool srgb, void (*log)(const char*, ...)) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = kW; d.Height = kH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(g_tex, &rv, &g_rtv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(g_tex, &sv, &g_srv);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)kW, (float)kH);
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    g_fTitle = io.Fonts->AddFontFromFileTTF((dir + "georgiab.ttf").c_str(), 40.0f);
    g_fText = io.Fonts->AddFontFromFileTTF((dir + "georgia.ttf").c_str(), 36.0f);
    g_fSmall = io.Fonts->AddFontFromFileTTF((dir + "georgia.ttf").c_str(), 27.0f);
    if (!g_fTitle || !g_fText || !g_fSmall) { log("MENU Georgia not found in %s, using the built-in font", dir.c_str()); io.Fonts->Clear(); g_fTitle = g_fText = g_fSmall = io.Fonts->AddFontDefault(); }
    if (!ImGui_ImplDX11_Init(dev, ctx)) return false;
    log("MENU panel ready %dx%d", kW, kH);
    return g_tex && g_rtv && g_srv;
}

static ImU32 Col(float r, float g, float b, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }
static void Text(ImDrawList* dl, ImFont* f, float x, float y, ImU32 c, const char* s) { dl->AddText(f, f->LegacySize, ImVec2(x, y), c, s); }
static float Width(ImFont* f, const char* s) { return f->CalcTextSizeA(f->LegacySize, FLT_MAX, 0, s).x; }

// A row box like the game's: dark grey fill, thin grey border; the chosen one gets a bright white outline with a soft glow.
static void RowBox(ImDrawList* dl, float x0, float y0, float x1, float y1, bool on) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), on ? Col(0.16f, 0.16f, 0.16f, 0.95f) : Col(0.09f, 0.09f, 0.09f, 0.90f));
    if (on) {
        for (int i = 3; i >= 1; i--) dl->AddRect(ImVec2(x0 - i * 2, y0 - i * 2), ImVec2(x1 + i * 2, y1 + i * 2), Col(1, 1, 1, 0.07f * (4 - i)), 0, 0, 2.0f);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Col(1, 1, 1, 0.95f), 0, 0, 2.0f);
    } else dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Col(0.45f, 0.45f, 0.45f, 0.55f), 0, 0, 1.0f);
}

// Fills the texture and returns its view, or nullptr when the menu is hidden or cannot be drawn.
ID3D11ShaderResourceView* DrawMenu(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool srgb, void (*log)(const char*, ...), MenuSnapshot* outPlacement) {
    MenuSnapshot m;
    { std::lock_guard<std::mutex> lk(g_menuMx); m = g_menu; }
    *outPlacement = m;
    if (!m.visible || g_failed) return nullptr;
    if (!g_init) { g_init = true; if (!Init(dev, ctx, srgb, log)) { g_failed = true; log("MENU panel could not be made"); return nullptr; } }
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 white = Col(0.95f, 0.95f, 0.95f, 1), grey = Col(0.70f, 0.70f, 0.70f, 0.90f), red = Col(0.55f, 0.08f, 0.08f, 1);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)kW, (float)kH), Col(0.02f, 0.02f, 0.02f, 0.94f));
    // Header: title left, version right, dark red rule.
    Text(dl, g_fTitle, 48, 22, white, "AI LIMIT VR");
    Text(dl, g_fSmall, kW - 48 - Width(g_fSmall, m.version), 32, grey, m.version);
    dl->AddLine(ImVec2(0, 84), ImVec2((float)kW, 84), red, 2.0f);
    float y = 104;
    if (m.banner[0]) { dl->AddText(g_fSmall, g_fSmall->LegacySize, ImVec2(48, y), Col(1, 0.45f, 0.4f, 1), m.banner, nullptr, kW - 96.0f); y += 70; }
    // Tabs along the top, like the game's pause menu (STATUS, EQUIPMENT, ...), with the bumper hints at the ends.
    float tx0 = 120, tx1 = kW - 120.0f, tw = m.tabCount ? (tx1 - tx0) / m.tabCount : 0;
    Text(dl, g_fSmall, 48, y + 12, grey, "LB");
    Text(dl, g_fSmall, kW - 48 - Width(g_fSmall, "RB"), y + 12, grey, "RB");
    for (int i = 0; i < m.tabCount; i++) {
        bool on = i == m.tab;
        float cx = tx0 + tw * (i + 0.5f);
        Text(dl, g_fText, cx - Width(g_fText, m.tabs[i]) * 0.5f, y + 6, on ? white : grey, m.tabs[i]);
        if (on) dl->AddLine(ImVec2(cx - Width(g_fText, m.tabs[i]) * 0.5f, y + 52), ImVec2(cx + Width(g_fText, m.tabs[i]) * 0.5f, y + 52), white, 2.0f);
    }
    // Rows below: label left, value right with < > like the game's option rows.
    float x0 = 60, x1 = kW - 60.0f, ry = y + 76;
    for (int i = 0; i < m.rowCount; i++) {
        bool on = i == m.row;
        RowBox(dl, x0, ry, x1, ry + 50, on);
        ImU32 c = on ? white : grey;
        Text(dl, g_fText, x0 + 24, ry + 6, c, m.rows[i].label);
        if (m.rows[i].value[0]) {
            float vw = Width(g_fText, m.rows[i].value), vx = x1 - 230 - vw * 0.5f;
            Text(dl, g_fText, vx, ry + 6, c, m.rows[i].value);
            if (!m.rows[i].button) { Text(dl, g_fText, x1 - 420, ry + 6, c, "<"); Text(dl, g_fText, x1 - 60, ry + 6, c, ">"); }
        }
        ry += 58;
    }
    // Footer: dark red rule, explanation bottom left, button hints bottom right.
    dl->AddLine(ImVec2(0, kH - 170.0f), ImVec2((float)kW, kH - 170.0f), red, 2.0f);
    dl->AddText(g_fSmall, g_fSmall->LegacySize, ImVec2(48, kH - 155.0f), white, m.explain, nullptr, kW - 96.0f);
    Text(dl, g_fSmall, kW - 48 - Width(g_fSmall, m.footer), kH - 42.0f, grey, m.footer);
    ImGui::Render();
    float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g_rtv, clear);
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)kW, (float)kH, 0, 1}; ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ID3D11RenderTargetView* none = nullptr; ctx->OMSetRenderTargets(1, &none, nullptr);
    return g_srv;
}
ID3D11Texture2D* MenuTexture() { return g_tex; }
float MenuAspect() { return (float)kH / kW; }

}  // namespace aixr
