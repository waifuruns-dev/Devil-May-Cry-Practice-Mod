// dinput8.dll proxy: the game loads this DLL thinking it is DirectInput.
// Every real call is forwarded to the system's dinput8.dll.
#include <windows.h>

#include "input_block.h"

namespace {
HMODULE g_real = nullptr;

FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}
}  // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPVOID outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPVOID);
    auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
    HRESULT hr = fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
    // Lets the practice menu hide its key presses from the game.
    if (SUCCEEDED(hr) && out) input_block::OnDirectInputCreated(*out);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = reinterpret_cast<Fn>(Real("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

}  // extern "C"
