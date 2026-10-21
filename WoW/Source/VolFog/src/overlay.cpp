#include "overlay.h"

#include "config.h"
#include "d3d9_wrap.h"
#include "hooks.h"
#include "log.h"
#include "settings_panel.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include <algorithm>

#if defined(_WIN32)
#include "imgui_impl_win32.h"

#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT msg, WPARAM wParam, LPARAM lParam);
#else
// CHANGED (wowdecomp): the Linux client has an SDL window, not a Win32 one. Its SDL events are translated into the
// Win32 messages below so the hotkey and button-ownership logic is shared; ImGui reads the SDL events themselves.
#include "imgui_impl_sdl2.h"

#include <SDL.h>

enum : UINT
{
    WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_DEADCHAR = 0x0103, WM_SYSKEYDOWN = 0x0104,
    WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106, WM_SYSDEADCHAR = 0x0107, WM_IME_COMPOSITION = 0x010F,
    WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203,
    WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207,
    WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A, WM_XBUTTONDOWN = 0x020B,
    WM_XBUTTONUP = 0x020C, WM_XBUTTONDBLCLK = 0x020D, WM_MOUSEHWHEEL = 0x020E, WM_MOUSELAST = 0x020E,
    WM_IME_CHAR = 0x0286
};
constexpr WORD XBUTTON1 = 1;
constexpr WORD XBUTTON2 = 2;
#define GET_XBUTTON_WPARAM(w) HIWORD(w)
#define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))
#ifndef MAKELPARAM
#define MAKELPARAM(l, h) (static_cast<LPARAM>(static_cast<DWORD>((static_cast<WORD>(l)) | (static_cast<DWORD>(static_cast<WORD>(h)) << 16))))
#endif
#endif

namespace
{
constexpr LPARAM kKeyWasDownBit = static_cast<LPARAM>(1) << 30;
constexpr DWORD kMaxRenderTargets = 4;
constexpr DWORD kAllColorChannels =
    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
constexpr float kPanelFontPixels = 16.0f;
constexpr float kReferenceBackBufferHeight = 1080.0f;
constexpr float kMinFrameSeconds = 1.0f / 1000.0f;
constexpr int kMaxLoggedShortcutKeys = 12;

enum DrawSkip : unsigned
{
    kDeviceNotReady = 1u << 0,
    kNoBackBuffer = 1u << 1,
    kNoStateCapture = 1u << 2,
};

struct OverlayState
{
    IDirect3DDevice9* device = nullptr;
    bool visible = false;
    bool failed = false;
    unsigned swallowedKey = 0;
    UINT backBufferWidth = 0;
    UINT backBufferHeight = 0;
    float scale = 0.0f;
    SettingsPanel panel;
    bool keyInputLogged = false;
    bool drawLogged = false;
    int loggedShortcutKeys = 0;
    unsigned loggedDrawSkips = 0;
    unsigned clientHeldButtons = 0;
    unsigned panelHeldButtons = 0;
    bool releaseFocusOnNextFrame = false;
    IDirect3DStateBlock9* deviceStateWhileVisible = nullptr;
};

struct OverlayFrame
{
    IDirect3DSurface9* backBuffer;
    IDirect3DStateBlock9* state;
    IDirect3DSurface9* targets[kMaxRenderTargets];
    IDirect3DSurface9* depth;
    IDirect3DVertexBuffer9* stream;
    UINT streamOffset;
    UINT streamStride;
    IDirect3DIndexBuffer9* indices;
    D3DVIEWPORT9 viewport;
    RECT scissor;
    bool captured;
    bool sceneOpen;
};

#if defined(_WIN32)
struct SubclassedWindow
{
    HWND window = nullptr;
    WNDPROC clientProc = nullptr;
    bool unicode = false;
};

#endif

OverlayState g_overlay;
#if defined(_WIN32)
SubclassedWindow g_subclass;
#else
struct SubclassedWindow
{
    HWND window = nullptr;
};
// The SDL window the overlay reads events for (there is no window procedure to chain on Linux).
SubclassedWindow g_subclass;
const SDL_Event* g_currentEvent = nullptr;
#endif

#if defined(_WIN32)
LRESULT CALLBACK OverlayWindowProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam);

LONG_PTR CurrentWindowProc(const SubclassedWindow& s)
{
    return s.unicode ? GetWindowLongPtrW(s.window, GWLP_WNDPROC) : GetWindowLongPtrA(s.window, GWLP_WNDPROC);
}

LONG_PTR ReplaceWindowProc(HWND window, bool unicode, LONG_PTR proc)
{
    return unicode ? SetWindowLongPtrW(window, GWLP_WNDPROC, proc) : SetWindowLongPtrA(window, GWLP_WNDPROC, proc);
}

bool RestoreClientWindowProc()
{
    if (!g_subclass.window)
        return true;
    if (CurrentWindowProc(g_subclass) != reinterpret_cast<LONG_PTR>(&OverlayWindowProc))
        return false;
    ReplaceWindowProc(g_subclass.window, g_subclass.unicode, reinterpret_cast<LONG_PTR>(g_subclass.clientProc));
    g_subclass = SubclassedWindow();
    return true;
}

bool ChainOverlayWindowProc(HWND window)
{
    if (g_subclass.window == window)
        return true;
    if (!RestoreClientWindowProc())
        return false;
    const bool unicode = IsWindowUnicode(window) != FALSE;
    const LONG_PTR clientProc = ReplaceWindowProc(window, unicode, reinterpret_cast<LONG_PTR>(&OverlayWindowProc));
    if (!clientProc)
        return false;
    g_subclass.window = window;
    g_subclass.clientProc = reinterpret_cast<WNDPROC>(clientProc);
    g_subclass.unicode = unicode;
    return true;
}
#else
bool RestoreClientWindowProc()
{
    g_subclass = SubclassedWindow();
    return true;
}

bool ChainOverlayWindowProc(HWND window)
{
    g_subclass.window = window;
    return true;
}
#endif

POINT ClientToBackBuffer(HWND window, POINT p)
{
    RECT client = {};
#if defined(_WIN32)
    if (!g_overlay.backBufferWidth || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return p;
#else
    int width = 0, height = 0;
    if (window)
        SDL_GetWindowSize(reinterpret_cast<SDL_Window*>(window), &width, &height);
    client.right = width;
    client.bottom = height;
    if (!g_overlay.backBufferWidth || client.right <= 0 || client.bottom <= 0)
        return p;
#endif
    return {MulDiv(p.x, static_cast<int>(g_overlay.backBufferWidth), client.right),
            MulDiv(p.y, static_cast<int>(g_overlay.backBufferHeight), client.bottom)};
}

LPARAM InBackBufferPixels(HWND window, UINT msg, LPARAM lParam)
{
    if (msg != WM_MOUSEMOVE)
        return lParam;
    const POINT p = ClientToBackBuffer(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    return MAKELPARAM(p.x, p.y);
}

void FeedCursorPosition(HWND window)
{
    POINT p;
#if defined(_WIN32)
    if (!window || !GetCursorPos(&p) || !ScreenToClient(window, &p))
        return;
#else
    int x = 0, y = 0;
    if (!window)
        return;
    SDL_GetMouseState(&x, &y);
    p = {x, y};
#endif
    p = ClientToBackBuffer(window, p);
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
}

bool ModifierDown(int virtualKey)
{
#if defined(_WIN32)
    return GetKeyState(virtualKey) < 0;
#else
    // The modifiers that went with the event being handled; SDL's live state may already be past it.
    const SDL_Keymod mods = g_currentEvent && (g_currentEvent->type == SDL_KEYDOWN || g_currentEvent->type == SDL_KEYUP)
                                ? static_cast<SDL_Keymod>(g_currentEvent->key.keysym.mod)
                                : SDL_GetModState();
    switch (virtualKey)
    {
    case VK_CONTROL:
        return (mods & KMOD_CTRL) != 0;
    case VK_SHIFT:
        return (mods & KMOD_SHIFT) != 0;
    case VK_MENU:
        return (mods & KMOD_ALT) != 0;
    case VK_LWIN:
        return (mods & KMOD_LGUI) != 0;
    case VK_RWIN:
        return (mods & KMOD_RGUI) != 0;
    default:
        return false;
    }
#endif
}

void FeedModifierState(ImGuiIO& io)
{
    io.AddKeyEvent(ImGuiMod_Ctrl, ModifierDown(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, ModifierDown(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt, ModifierDown(VK_MENU));
    io.AddKeyEvent(ImGuiMod_Super, ModifierDown(VK_LWIN) || ModifierDown(VK_RWIN));
}

void SetVisible(bool visible)
{
    if (visible == g_overlay.visible || !ImGui::GetCurrentContext())
        return;
    g_overlay.visible = visible;
    g_overlay.releaseFocusOnNextFrame = true;
    VF_LOG_INFO("overlay %s", visible ? "shown" : "hidden");
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    if (visible)
        FeedCursorPosition(g_subclass.window);
}

bool IsKeyDownMessage(UINT msg)
{
    return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
}

bool IsKeyUpMessage(UINT msg)
{
    return msg == WM_KEYUP || msg == WM_SYSKEYUP;
}

bool IsCharacterMessage(UINT msg)
{
    return msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR;
}

bool IsMouseClickOrWheelMessage(UINT msg)
{
    return msg > WM_MOUSEMOVE && msg <= WM_MOUSELAST;
}

bool IsImeMessage(UINT msg)
{
    return msg == WM_IME_COMPOSITION || msg == WM_IME_CHAR;
}

unsigned MouseButtonBit(UINT msg, WPARAM wParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return 1u << 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return 1u << 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return 1u << 2;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 1u << 3 : 1u << 4;
    default:
        return 0;
    }
}

bool IsMouseButtonUp(UINT msg)
{
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
}

bool KeepsButtonWithItsDown(UINT msg, WPARAM wParam, bool takenByPanel)
{
    const unsigned button = MouseButtonBit(msg, wParam);
    if (!button)
        return takenByPanel;
    if (IsMouseButtonUp(msg))
    {
        const bool clientOwned = (g_overlay.clientHeldButtons & button) != 0;
        const bool panelOwned = (g_overlay.panelHeldButtons & button) != 0;
        g_overlay.clientHeldButtons &= ~button;
        g_overlay.panelHeldButtons &= ~button;
        return clientOwned ? false : panelOwned || takenByPanel;
    }
    if (takenByPanel)
    {
        g_overlay.panelHeldButtons |= button;
        g_overlay.clientHeldButtons &= ~button;
    }
    else
    {
        g_overlay.clientHeldButtons |= button;
        g_overlay.panelHeldButtons &= ~button;
    }
    return takenByPanel;
}

bool IsHotkeyKey(const Hotkey& key, WPARAM wParam)
{
    if (wParam == key.virtualKey)
        return true;
    return key.ctrl && wParam == VK_CANCEL && (key.virtualKey == VK_PAUSE || key.virtualKey == VK_SCROLL);
}

bool HotkeyPressed(const Hotkey& key, UINT msg, WPARAM wParam)
{
    return IsKeyDownMessage(msg) && IsHotkeyKey(key, wParam) && ModifierDown(VK_CONTROL) == key.ctrl &&
           ModifierDown(VK_SHIFT) == key.shift && ModifierDown(VK_MENU) == key.alt;
}

bool IsTypedKey(unsigned virtualKey)
{
    return (virtualKey >= '0' && virtualKey <= '9') || (virtualKey >= 'A' && virtualKey <= 'Z') ||
           (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_DIVIDE) ||
           (virtualKey >= VK_OEM_1 && virtualKey <= VK_OEM_102) || virtualKey == VK_SPACE;
}

bool IsModifierKey(unsigned virtualKey)
{
    return virtualKey == VK_CONTROL || virtualKey == VK_SHIFT || virtualKey == VK_MENU ||
           (virtualKey >= VK_LSHIFT && virtualKey <= VK_RMENU) || virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

void LogShortcutKeyArrival(const Hotkey& hotkey, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!IsKeyDownMessage(msg) || (lParam & kKeyWasDownBit))
        return;
    if (!g_overlay.keyInputLogged)
    {
        g_overlay.keyInputLogged = true;
        VF_LOG_INFO("overlay: key presses reach the game window");
    }
    const unsigned virtualKey = static_cast<unsigned>(wParam);
    const Hotkey pressed = {virtualKey, ModifierDown(VK_CONTROL), ModifierDown(VK_SHIFT), ModifierDown(VK_MENU)};
    const bool shortcut = (pressed.ctrl || pressed.alt || pressed.shift) && !IsTypedKey(virtualKey);
    if ((virtualKey != hotkey.virtualKey && !shortcut) || IsModifierKey(virtualKey) ||
        g_overlay.loggedShortcutKeys >= kMaxLoggedShortcutKeys)
        return;
    ++g_overlay.loggedShortcutKeys;
    VF_LOG_INFO("overlay: %s pressed (OverlayKey is %s)", HotkeyName(pressed).c_str(), HotkeyName(hotkey).c_str());
}

void LogDrawSkip(DrawSkip reason, const char* what, HRESULT hr)
{
    if (g_overlay.loggedDrawSkips & reason)
        return;
    g_overlay.loggedDrawSkips |= reason;
    VF_LOG_INFO("overlay not drawn: %s (0x%08X)", what, static_cast<unsigned>(hr));
}

bool TakesHotkey(const Hotkey& key, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (HotkeyPressed(key, msg, wParam))
    {
        if (!(lParam & kKeyWasDownBit))
            SetVisible(!g_overlay.visible);
        g_overlay.swallowedKey = static_cast<unsigned>(wParam);
        return true;
    }
    if (!g_overlay.swallowedKey)
        return false;
    if (IsKeyUpMessage(msg) && (wParam == g_overlay.swallowedKey || IsHotkeyKey(key, wParam)))
    {
        g_overlay.swallowedKey = 0;
        return true;
    }
    return IsCharacterMessage(msg);
}

bool PanelWantsMessage(UINT msg)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (IsMouseClickOrWheelMessage(msg))
        return io.WantCaptureMouse;
    if (IsKeyDownMessage(msg) || IsCharacterMessage(msg))
        return io.WantCaptureKeyboard;
    return false;
}

#if !defined(_WIN32)
// Hands ImGui the SDL event being handled, with mouse positions in back-buffer pixels like the Win32 path.
LRESULT FeedPanelEvent(HWND window, UINT msg, LPARAM scaled)
{
    if (!g_currentEvent)
        return 0;
    SDL_Event event = *g_currentEvent;
    if (event.type == SDL_MOUSEMOTION)
    {
        event.motion.x = GET_X_LPARAM(scaled);
        event.motion.y = GET_Y_LPARAM(scaled);
    }
    else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
    {
        const POINT p = ClientToBackBuffer(window, {event.button.x, event.button.y});
        event.button.x = p.x;
        event.button.y = p.y;
    }
    return ImGui_ImplSDL2_ProcessEvent(&event) ? 1 : 0;
}
#endif

bool PanelTakesMessage(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const Config& cfg = GlobalConfig().Get();
    if (!cfg.overlay)
    {
        SetVisible(false);
        return false;
    }
    LogShortcutKeyArrival(cfg.overlayKey, msg, wParam, lParam);
    if (TakesHotkey(cfg.overlayKey, msg, wParam, lParam))
        return true;
    if (!g_overlay.visible)
        return false;
    if (IsImeMessage(msg) && !ImGui::GetIO().WantTextInput)
        return false;
    const LPARAM scaled = InBackBufferPixels(window, msg, lParam);
#if defined(_WIN32)
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(window, msg, wParam, scaled);
#else
    const LRESULT handled = FeedPanelEvent(window, msg, scaled);
#endif
    if (IsImeMessage(msg))
        return handled != 0;
    return PanelWantsMessage(msg);
}

bool OverlayTakesMessage(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_overlay.failed || !g_overlay.device)
        return false;
    return KeepsButtonWithItsDown(msg, wParam, PanelTakesMessage(window, msg, wParam, lParam));
}

int OverlayFaultFilter(unsigned code, const char* where)
{
    VF_LOG_ERROR("exception 0x%08X in the overlay %s; overlay disabled for this session", code, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool OverlayTakesMessageGuarded(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    VF_TRY
    {
        return OverlayTakesMessage(window, msg, wParam, lParam);
    }
    VF_EXCEPT(OverlayFaultFilter(GetExceptionCode(), "window message"))
    {
        g_overlay.failed = true;
        g_overlay.visible = false;
        return false;
    }
}

#if defined(_WIN32)
LRESULT CALLBACK OverlayWindowProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const SubclassedWindow subclass = g_subclass;
    if (window == subclass.window && OverlayTakesMessageGuarded(window, msg, wParam, lParam))
        return 0;
    if (msg == WM_NCDESTROY && window == subclass.window)
        g_subclass = SubclassedWindow();
    return subclass.unicode ? CallWindowProcW(subclass.clientProc, window, msg, wParam, lParam)
                            : CallWindowProcA(subclass.clientProc, window, msg, wParam, lParam);
}
#endif

void ConfigureImGui()
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImFontConfig font;
    font.SizePixels = kPanelFontPixels;
    io.Fonts->AddFontDefaultVector(&font);
    g_overlay.scale = 0.0f;
}

void ApplyScale(float scale)
{
    if (scale == g_overlay.scale)
        return;
    g_overlay.scale = scale;
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui::GetStyle() = style;
}

void BuildPanelFrame(const D3DSURFACE_DESC& backBuffer)
{
    ApplyScale(std::max(1.0f, static_cast<float>(backBuffer.Height) / kReferenceBackBufferHeight));
    ImGui_ImplDX9_NewFrame();
#if defined(_WIN32)
    ImGui_ImplWin32_NewFrame();
#else
    ImGui_ImplSDL2_NewFrame();
#endif
    ImGuiIO& io = ImGui::GetIO();
    FeedModifierState(io);
    io.DisplaySize = ImVec2(static_cast<float>(backBuffer.Width), static_cast<float>(backBuffer.Height));
#if !defined(_WIN32)
    // The SDL backend reports the window size and pointer in window units; the panel works in back-buffer pixels.
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    FeedCursorPosition(g_subclass.window);
#endif
    io.DeltaTime = std::max(io.DeltaTime, kMinFrameSeconds);
    ImGui::NewFrame();
    if (g_overlay.releaseFocusOnNextFrame)
    {
        g_overlay.releaseFocusOnNextFrame = false;
        ImGui::SetWindowFocus(nullptr);
    }
    bool open = true;
    g_overlay.panel.Draw(GlobalConfig(), LastFogFrameStatus(), LastWaterFrameStatus(), CurrentMultisamplingStatus(),
                         open);
    ImGui::Render();
    if (!open)
        SetVisible(false);
}

bool EnsureDeviceStateBlock(IDirect3DDevice9* device)
{
    if (g_overlay.deviceStateWhileVisible)
        return true;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &g_overlay.deviceStateWhileVisible)))
        return true;
    g_overlay.deviceStateWhileVisible = nullptr;
    return false;
}

bool CaptureDeviceState(IDirect3DDevice9* device, OverlayFrame& frame)
{
    if (!EnsureDeviceStateBlock(device) || FAILED(g_overlay.deviceStateWhileVisible->Capture()))
        return false;
    frame.state = g_overlay.deviceStateWhileVisible;
    for (DWORD i = 0; i < kMaxRenderTargets; ++i)
        device->GetRenderTarget(i, &frame.targets[i]);
    device->GetDepthStencilSurface(&frame.depth);
    device->GetStreamSource(0, &frame.stream, &frame.streamOffset, &frame.streamStride);
    device->GetIndices(&frame.indices);
    device->GetViewport(&frame.viewport);
    device->GetScissorRect(&frame.scissor);
    frame.captured = frame.targets[0] != nullptr;
    return frame.captured;
}

void RestoreDeviceState(IDirect3DDevice9* device, const OverlayFrame& frame)
{
    for (DWORD i = 0; i < kMaxRenderTargets; ++i)
        if (frame.targets[i])
            device->SetRenderTarget(i, frame.targets[i]);
    device->SetDepthStencilSurface(frame.depth);
    frame.state->Apply();
    device->SetStreamSource(0, frame.stream, frame.streamOffset, frame.streamStride);
    device->SetIndices(frame.indices);
    device->SetViewport(&frame.viewport);
    device->SetScissorRect(&frame.scissor);
}

template <typename T>
void ReleaseReference(T*& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

void EndOverlayFrame(IDirect3DDevice9* device, OverlayFrame& frame)
{
    if (frame.sceneOpen)
        device->EndScene();
    if (frame.captured)
        RestoreDeviceState(device, frame);
    frame.sceneOpen = false;
    frame.captured = false;
    for (IDirect3DSurface9*& target : frame.targets)
        ReleaseReference(target);
    ReleaseReference(frame.depth);
    ReleaseReference(frame.stream);
    ReleaseReference(frame.indices);
    frame.state = nullptr;
    ReleaseReference(frame.backBuffer);
}

void SetStateImGuiLeavesAlone(IDirect3DDevice9* device)
{
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kAllColorChannels);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_WRAP0, 0);
    device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
}

void RenderPanel(IDirect3DDevice9* device, OverlayFrame& frame)
{
    if (!CaptureDeviceState(device, frame))
    {
        LogDrawSkip(kNoStateCapture, "the device state could not be captured", E_FAIL);
        return;
    }
    frame.sceneOpen = SUCCEEDED(device->BeginScene());
    device->SetRenderTarget(0, frame.backBuffer);
    for (DWORD i = 1; i < kMaxRenderTargets; ++i)
        if (frame.targets[i])
            device->SetRenderTarget(i, nullptr);
    device->SetDepthStencilSurface(nullptr);
    SetStateImGuiLeavesAlone(device);
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void DrawOverlayFrame(IDirect3DDevice9* device, OverlayFrame& frame)
{
    if (!GlobalConfig().Get().overlay)
    {
        SetVisible(false);
        return;
    }
    const HRESULT cooperativeLevel = device->TestCooperativeLevel();
    if (cooperativeLevel != D3D_OK)
    {
        LogDrawSkip(kDeviceNotReady, "the device is not ready", cooperativeLevel);
        return;
    }
    const HRESULT backBufferResult = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &frame.backBuffer);
    if (FAILED(backBufferResult))
    {
        LogDrawSkip(kNoBackBuffer, "no back buffer", backBufferResult);
        return;
    }
    D3DSURFACE_DESC desc = {};
    frame.backBuffer->GetDesc(&desc);
    g_overlay.backBufferWidth = desc.Width;
    g_overlay.backBufferHeight = desc.Height;
    BuildPanelFrame(desc);
    RenderPanel(device, frame);
    if (!g_overlay.drawLogged)
    {
        g_overlay.drawLogged = true;
        VF_LOG_INFO("overlay drawn over the %ux%u back buffer", desc.Width, desc.Height);
    }
}

void EndOverlayFrameGuarded(IDirect3DDevice9* device, OverlayFrame& frame)
{
    VF_TRY
    {
        EndOverlayFrame(device, frame);
    }
    VF_EXCEPT(OverlayFaultFilter(GetExceptionCode(), "frame cleanup"))
    {
        g_overlay.failed = true;
        g_overlay.visible = false;
    }
}

void DrawOverlayGuarded(IDirect3DDevice9* device)
{
    OverlayFrame frame = {};
    VF_TRY
    {
        DrawOverlayFrame(device, frame);
    }
    VF_EXCEPT(OverlayFaultFilter(GetExceptionCode(), "frame"))
    {
        g_overlay.failed = true;
        g_overlay.visible = false;
    }
    EndOverlayFrameGuarded(device, frame);
}

void ShutDownImGui()
{
    ImGui_ImplDX9_Shutdown();
#if defined(_WIN32)
    ImGui_ImplWin32_Shutdown();
#else
    ImGui_ImplSDL2_Shutdown();
#endif
    ImGui::DestroyContext();
}
}

void AttachOverlay(IDirect3DDevice9* device, HWND window)
{
    if (g_overlay.failed || !device || !window)
        return;
    if (g_overlay.device)
        DetachOverlay(g_overlay.device);
    if (!ChainOverlayWindowProc(window))
    {
        VF_LOG_ERROR("overlay: window %p could not be subclassed; overlay off", static_cast<void*>(window));
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGui();
#if defined(_WIN32)
    if (!ImGui_ImplWin32_Init(window))
#else
    if (!ImGui_ImplSDL2_InitForD3D(reinterpret_cast<SDL_Window*>(window)))
#endif
    {
        ImGui::DestroyContext();
        RestoreClientWindowProc();
        VF_LOG_ERROR("overlay: the ImGui platform backend failed to start; overlay off");
        return;
    }
    ImGui_ImplDX9_Init(device);
    g_overlay.device = device;
    VF_LOG_INFO("overlay attached to window %p; %s opens it", static_cast<void*>(window),
                HotkeyName(GlobalConfig().Get().overlayKey).c_str());
}

void DetachOverlay(IDirect3DDevice9* device)
{
    if (!device || device != g_overlay.device)
        return;
    SetVisible(false);
    ReleaseReference(g_overlay.deviceStateWhileVisible);
    ShutDownImGui();
    const bool failed = g_overlay.failed;
    const SettingsPanel panel = g_overlay.panel;
    g_overlay = OverlayState();
    g_overlay.failed = failed;
    g_overlay.panel = panel;
    if (!RestoreClientWindowProc())
        VF_LOG_INFO("overlay detached; the window procedure stays chained because another hook follows it");
}

void ReleaseOverlayDeviceObjects(IDirect3DDevice9* device)
{
    if (!device || device != g_overlay.device)
        return;
    ReleaseReference(g_overlay.deviceStateWhileVisible);
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void DrawOverlay(IDirect3DDevice9* device)
{
    if (!device || device != g_overlay.device)
        return;
    if (g_overlay.visible && !g_overlay.failed)
        DrawOverlayGuarded(device);
    if (!g_overlay.visible || g_overlay.failed)
        ReleaseReference(g_overlay.deviceStateWhileVisible);
}

bool OverlayVisible()
{
    return g_overlay.visible;
}

PanelPlacement OverlayPanelPlacement()
{
    return g_overlay.panel.Placement();
}

#if !defined(_WIN32)
namespace
{
// Windows virtual-key code for an SDL key, which is what OverlayKey and the rest of CoAVolFog's hotkeys use.
unsigned VirtualKeyOf(const SDL_Keysym& key)
{
    const SDL_Keycode sym = key.sym;
    if (sym >= SDLK_a && sym <= SDLK_z)
        return 'A' + static_cast<unsigned>(sym - SDLK_a);
    if (sym >= SDLK_0 && sym <= SDLK_9)
        return '0' + static_cast<unsigned>(sym - SDLK_0);
    const SDL_Scancode s = key.scancode;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12)
        return VK_F1 + static_cast<unsigned>(s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24)
        return VK_F1 + 12 + static_cast<unsigned>(s - SDL_SCANCODE_F13);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9)
        return VK_NUMPAD0 + 1 + static_cast<unsigned>(s - SDL_SCANCODE_KP_1);
    switch (s)
    {
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;
    case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
    case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_RETURN: return VK_RETURN;
    case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_LGUI: return VK_LWIN;
    case SDL_SCANCODE_RGUI: return VK_RWIN;
    case SDL_SCANCODE_APPLICATION: return VK_APPS;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;
    case SDL_SCANCODE_LSHIFT: return VK_SHIFT;
    case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
    case SDL_SCANCODE_LCTRL: return VK_CONTROL;
    case SDL_SCANCODE_RCTRL: return VK_CONTROL;
    case SDL_SCANCODE_LALT: return VK_MENU;
    case SDL_SCANCODE_RALT: return VK_MENU;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;
    case SDL_SCANCODE_AUDIOMUTE: return VK_VOLUME_MUTE;
    case SDL_SCANCODE_VOLUMEDOWN: return VK_VOLUME_DOWN;
    case SDL_SCANCODE_VOLUMEUP: return VK_VOLUME_UP;
    case SDL_SCANCODE_AUDIONEXT: return VK_MEDIA_NEXT_TRACK;
    case SDL_SCANCODE_AUDIOPREV: return VK_MEDIA_PREV_TRACK;
    case SDL_SCANCODE_AUDIOSTOP: return VK_MEDIA_STOP;
    case SDL_SCANCODE_AUDIOPLAY: return VK_MEDIA_PLAY_PAUSE;
    default: return 0;
    }
}

UINT ButtonMessage(Uint8 button, bool down, WPARAM& wParam)
{
    wParam = 0;
    switch (button)
    {
    case SDL_BUTTON_LEFT:
        return down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
    case SDL_BUTTON_RIGHT:
        return down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    case SDL_BUTTON_MIDDLE:
        return down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
    case SDL_BUTTON_X1:
    case SDL_BUTTON_X2:
        wParam = static_cast<WPARAM>(button == SDL_BUTTON_X1 ? XBUTTON1 : XBUTTON2) << 16;
        return down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
    default:
        return 0;
    }
}

// The Win32 message an SDL input event stands for; false for events the overlay never takes.
bool TranslateEvent(const SDL_Event& e, UINT& msg, WPARAM& wParam, LPARAM& lParam)
{
    wParam = 0;
    lParam = 0;
    switch (e.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        wParam = VirtualKeyOf(e.key.keysym);
        if (!wParam)
            return false;
        msg = e.type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP;
        if (e.key.repeat)
            lParam = kKeyWasDownBit;
        return true;
    case SDL_TEXTINPUT:
        msg = WM_CHAR;
        wParam = static_cast<unsigned char>(e.text.text[0]);
        return true;
    case SDL_TEXTEDITING:
        msg = WM_IME_COMPOSITION;
        return true;
    case SDL_MOUSEMOTION:
        msg = WM_MOUSEMOVE;
        lParam = MAKELPARAM(e.motion.x, e.motion.y);
        return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        msg = ButtonMessage(e.button.button, e.type == SDL_MOUSEBUTTONDOWN, wParam);
        lParam = MAKELPARAM(e.button.x, e.button.y);
        return msg != 0;
    case SDL_MOUSEWHEEL:
        msg = e.wheel.x && !e.wheel.y ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
        return true;
    default:
        return false;
    }
}

Uint32 EventWindowId(const SDL_Event& e)
{
    switch (e.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return e.key.windowID;
    case SDL_TEXTINPUT:
        return e.text.windowID;
    case SDL_TEXTEDITING:
        return e.edit.windowID;
    case SDL_MOUSEMOTION:
        return e.motion.windowID;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        return e.button.windowID;
    case SDL_MOUSEWHEEL:
        return e.wheel.windowID;
    case SDL_WINDOWEVENT:
        return e.window.windowID;
    default:
        return 0;
    }
}
}

// The Linux counterpart of the window procedure CoAVolFog chains on Windows: the client's SDL input loop offers
// every event here first and drops it when this returns nonzero.
extern "C" int vf_on_os_event(void* sdlEvent)
{
    if (!sdlEvent || !g_overlay.device || g_overlay.failed || !g_subclass.window)
        return 0;
    const SDL_Event& e = *static_cast<const SDL_Event*>(sdlEvent);
    SDL_Window* window = reinterpret_cast<SDL_Window*>(g_subclass.window);
    const Uint32 id = EventWindowId(e);
    if (id && id != SDL_GetWindowID(window))
        return 0;
    UINT msg = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    if (!TranslateEvent(e, msg, wParam, lParam))
    {
        // Focus and enter/leave keep ImGui's idea of the window current; the client still gets them.
        if (g_overlay.visible && e.type == SDL_WINDOWEVENT)
            ImGui_ImplSDL2_ProcessEvent(&e);
        return 0;
    }
    g_currentEvent = &e;
    const bool taken = OverlayTakesMessageGuarded(g_subclass.window, msg, wParam, lParam);
    g_currentEvent = nullptr;
    return taken ? 1 : 0;
}
#endif
