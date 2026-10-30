#include "hooks.h"
#include "config.h"
#include "log.h"
#include "overlay.h"
#include "mode.h"

#include <windows.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResize = nullptr;

bool g_imguiReady = false;
bool g_visible = true;
std::atomic<int> g_presentCalls{0};  // Present calls seen (table hook check)
bool g_keyWasDown = false;
HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void CreateRTV(IDXGISwapChain* swap) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void ReleaseRTV() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool InitImGui(IDXGISwapChain* swap) {
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
        dlog::Write("GetDevice failed: game swapchain isn't D3D11?");
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swap->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                      // don't write imgui.ini into the game folder
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;    // overlay is display-only
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.FontGlobalScale = g_config.fontScale;
    overlay::SetDisplayHeight(desc.BufferDesc.Height);  // UI scale for this window size
    overlay::LoadFonts();
    overlay::ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    CreateRTV(swap);

    g_visible = g_config.showOnStart;
    dlog::Write("ImGui initialized (%ux%u)", desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

void PollToggleKey() {
    // Only react while the game window has focus, so typing elsewhere doesn't toggle it.
    if (GetForegroundWindow() != g_hwnd) {
        g_keyWasDown = false;
        return;
    }
    bool down = (GetAsyncKeyState(g_config.toggleKey) & 0x8000) != 0;
    if (down && !g_keyWasDown) g_visible = !g_visible;
    g_keyWasDown = down;
}

bool g_resized = false;  // the game resized its back buffer: check the UI scale

// After a resolution change: rescale the UI, rebuilding the fonts at the new pixel size so text
// stays sharp.
void CheckUiScale(IDXGISwapChain* swap) {
    g_resized = false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || !overlay::SetDisplayHeight(desc.BufferDesc.Height)) return;
    ImGui::GetIO().Fonts->Clear();
    overlay::LoadFonts();
    overlay::ApplyStyle();
    ImGui_ImplDX11_InvalidateDeviceObjects();  // the font texture is rebuilt on the next frame
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    g_presentCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_imguiReady) g_imguiReady = InitImGui(swap);
    else if (g_resized) CheckUiScale(swap);

    if (g_imguiReady) {
        // F10: the whole mod on / off. Off = nothing below runs (plain DMC1), only its messages.
        mode::Update(GetForegroundWindow() == g_hwnd);
        const bool on = mode::On();
        if (on) {
            PollToggleKey();
            overlay::Update(GetForegroundWindow() == g_hwnd);
        }
        if (!g_rtv) CreateRTV(swap);

        if (((on && (g_visible || overlay::WantsDraw())) || mode::WantsDraw()) && g_rtv) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (on) overlay::Draw(g_visible);
            mode::Draw();
            ImGui::Render();

            // Save and restore the game's render targets so we don't disturb its next frame.
            ID3D11RenderTargetView* prevRtv = nullptr;
            ID3D11DepthStencilView* prevDsv = nullptr;
            g_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);

            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
            if (prevRtv) prevRtv->Release();
            if (prevDsv) prevDsv->Release();
        }
    }

    return g_origPresent(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swap, UINT count, UINT w, UINT h,
                                              DXGI_FORMAT fmt, UINT flags) {
    // The back buffer is about to be recreated; drop our view of it first.
    ReleaseRTV();
    g_resized = true;
    return g_origResize(swap, count, w, h, fmt, flags);
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Makes a throwaway device + swapchain just to find the Present/ResizeBuffers
// function addresses inside dxgi.dll, which the game's swapchain shares.
bool FindSwapChainFunctions(void*& present, void*& resize, void**& table) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DMC1PracticeDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    bool ok = SUCCEEDED(hr) && swap;
    if (ok) {
        void** vtable = *reinterpret_cast<void***>(swap);
        present = vtable[8];   // IDXGISwapChain::Present
        resize = vtable[13];   // IDXGISwapChain::ResizeBuffers
        table = vtable;        // shared by every swap chain of this kind (in dxgi.dll)
    } else {
        dlog::Write("Dummy D3D11 device failed (hr=0x%08lX)", (unsigned long)hr);
    }

    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

// Present is hooked in the swap chain's function table (the game calls Present through it), not by
// patching Present's code. Capture software (OBS game capture) patches Present's code, so this way
// the mod draws before it sees the frame: the overlay is always in the recording, even with OBS's
// "Capture third-party overlays" off. If the game's swap chain turns out to use another table,
// the code-patching hook is installed after all (InlineFallback).
void** g_swapVtable = nullptr;
void* g_presentFn = nullptr;
void* g_resizeFn = nullptr;

bool PatchSlot(void** slot, void* fn) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool InstallInline(void* present, void* resize) {
    if (MH_CreateHook(present, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
        MH_CreateHook(resize, reinterpret_cast<void*>(&HookedResizeBuffers), reinterpret_cast<void**>(&g_origResize)) != MH_OK) {
        dlog::Write("Couldn't create Present/ResizeBuffers hooks");
        return false;
    }
    if (MH_EnableHook(present) != MH_OK || MH_EnableHook(resize) != MH_OK) {
        dlog::Write("Couldn't enable hooks");
        return false;
    }
    dlog::Write("D3D11 hooks installed (code patch)");
    return true;
}

DWORD WINAPI InlineFallback(LPVOID) {
    Sleep(20000);
    if (g_presentCalls.load() > 0) return 0;
    dlog::Write("[Hooks] the game's Present doesn't go through the table hook: patching Present's code instead "
                "(the overlay may then be hidden by OBS's \"capture third-party overlays\" = off)");
    PatchSlot(&g_swapVtable[8], g_presentFn);
    PatchSlot(&g_swapVtable[13], g_resizeFn);
    InstallInline(g_presentFn, g_resizeFn);
    return 0;
}

}  // namespace

bool hooks::InstallD3D11() {
    void* present = nullptr;
    void* resize = nullptr;
    void** vtable = nullptr;
    if (!FindSwapChainFunctions(present, resize, vtable)) return false;

    // The keyboard hook (input_block.cpp) may have initialized MinHook already.
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        dlog::Write("MinHook failed to initialize");
        return false;
    }
    g_swapVtable = vtable;
    g_presentFn = present;
    g_resizeFn = resize;
    g_origPresent = reinterpret_cast<decltype(g_origPresent)>(present);
    g_origResize = reinterpret_cast<decltype(g_origResize)>(resize);
    if (!PatchSlot(&vtable[8], reinterpret_cast<void*>(&HookedPresent)) ||
        !PatchSlot(&vtable[13], reinterpret_cast<void*>(&HookedResizeBuffers))) {
        dlog::Write("[Hooks] table hook failed: patching Present's code instead");
        return InstallInline(present, resize);
    }
    dlog::Write("D3D11 hooks installed (swap chain table: drawn before capture software)");
    HANDLE t = CreateThread(nullptr, 0, &InlineFallback, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return true;
}

