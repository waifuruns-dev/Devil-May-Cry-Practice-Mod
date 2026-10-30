#include "input_block.h"
#include "log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>

namespace {

// {6F1D2B61-D5A0-11CF-BFC7-444553540000}, defined here so we don't need dxguid.lib.
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

CreateDeviceFn g_origCreateDevice = nullptr;
GetDeviceStateFn g_origGetState = nullptr;
GetDeviceDataFn g_origGetData = nullptr;

std::atomic<bool> g_blocked{false};
std::mutex g_mutex;
std::set<void*> g_keyboards;
bool g_deviceHooked = false;
bool g_createHooked = false;

bool IsKeyboard(void* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_keyboards.count(dev) != 0;
}

bool EnsureMinHook() {
    MH_STATUS s = MH_Initialize();
    return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
}

HRESULT STDMETHODCALLTYPE HookedGetState(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
    HRESULT hr = g_origGetState(dev, size, data);
    if (SUCCEEDED(hr) && g_blocked && data && IsKeyboard(dev)) memset(data, 0, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedGetData(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data,
                                        LPDWORD inOut, DWORD flags) {
    HRESULT hr = g_origGetData(dev, objSize, data, inOut, flags);
    // Still drain the buffer, but report no key events.
    if (SUCCEEDED(hr) && g_blocked && inOut && IsKeyboard(dev)) *inOut = 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateDevice(IDirectInput8W* di, REFGUID guid, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN outer) {
    HRESULT hr = g_origCreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, kSysKeyboard)) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_keyboards.insert(*out);
        }
        // Every DirectInput device shares the same implementation functions, so hook them once.
        if (!g_deviceHooked && EnsureMinHook()) {
            void** vt = *reinterpret_cast<void***>(*out);
            if (MH_CreateHook(vt[9], reinterpret_cast<void*>(&HookedGetState), reinterpret_cast<void**>(&g_origGetState)) == MH_OK &&
                MH_CreateHook(vt[10], reinterpret_cast<void*>(&HookedGetData), reinterpret_cast<void**>(&g_origGetData)) == MH_OK &&
                MH_EnableHook(vt[9]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK) {
                g_deviceHooked = true;
                dlog::Write("[Input] keyboard hooks installed");
            } else {
                dlog::Write("[Input] couldn't hook the keyboard device; menu keys will also reach the game");
            }
        }
    }
    return hr;
}

}  // namespace

void input_block::OnDirectInputCreated(void* directInput) {
    if (!directInput || g_createHooked || !EnsureMinHook()) return;
    void** vt = *reinterpret_cast<void***>(directInput);
    if (MH_CreateHook(vt[3], reinterpret_cast<void*>(&HookedCreateDevice), reinterpret_cast<void**>(&g_origCreateDevice)) == MH_OK &&
        MH_EnableHook(vt[3]) == MH_OK) {
        g_createHooked = true;
    } else {
        dlog::Write("[Input] couldn't hook CreateDevice");
    }
}

void input_block::SetBlocked(bool blocked) { g_blocked = blocked; }
