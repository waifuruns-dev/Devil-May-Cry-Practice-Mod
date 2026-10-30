#include "config.h"
#include "hooks.h"
#include "log.h"
#include "version.h"
#include "overlay.h"
#include "backup.h"

#include <windows.h>
#include <string>

namespace {

HMODULE g_self = nullptr;

std::wstring FolderOf(HMODULE mod) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
}

std::wstring ExeName() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

DWORD WINAPI InitThread(LPVOID) {
    const std::wstring folder = FolderOf(g_self);
    if (_wcsicmp(ExeName().c_str(), g_config.targetExe.c_str()) == 0)
        backup::BackUpSaves(folder);  // first: before the game reads (or writes) its save

    // Load any other mods listed in the INI (e.g. the camera mod renamed to dmc1_cam.dll),
    // so both can run at once even though both want to be dinput8.dll.
    for (const std::wstring& dll : g_config.chainLoad) {
        std::wstring full = folder + dll;
        if (LoadLibraryW(full.c_str())) dlog::Write("Chain-loaded %ls", dll.c_str());
        else dlog::Write("Couldn't chain-load %ls (error %lu)", dll.c_str(), GetLastError());
    }

    // The HD Collection folder also holds dmc2.exe, dmc3.exe and the launcher,
    // which would load this DLL too. Only draw the overlay inside the target game.
    if (_wcsicmp(ExeName().c_str(), g_config.targetExe.c_str()) != 0) {
        dlog::Write("Running in %ls, not %ls: overlay disabled", ExeName().c_str(), g_config.targetExe.c_str());
        return 0;
    }

    // Wait for the game to load D3D11 before hooking it.
    for (int i = 0; i < 300 && !GetModuleHandleW(L"d3d11.dll"); ++i) Sleep(100);

    overlay::Init();
    if (!hooks::InstallD3D11()) dlog::Write("Overlay disabled: D3D11 hook failed");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);

        const std::wstring folder = FolderOf(mod);
        const bool configOk = LoadConfig(folder + L"dmc1_practice.ini", g_config);
        dlog::g_verbose = g_config.verboseLog;

        // The launcher (and dmc2/dmc3) load this DLL too. They get their own log, so they never
        // overwrite the game's. The game's previous log is kept as dmc1_practice.prev.log, so the
        // log from a crashed session survives one relaunch.
        if (_wcsicmp(ExeName().c_str(), g_config.targetExe.c_str()) == 0) {
            const std::wstring log = folder + L"dmc1_practice.log";
            MoveFileExW(log.c_str(), (folder + L"dmc1_practice.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
            dlog::Open(log);
        } else {
            dlog::Open(folder + L"dmc1_practice_other.log");
        }
        // Which build of the game this is (addresses inside dmc1.exe differ between builds).
        {
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            GetFileAttributesExW(exePath, GetFileExInfoStandard, &fa);
            dlog::Write("DMC1 Practice Mod %s by waifu", kVersion);
            dlog::Write("Game: %ls, PE timestamp %08X, image size %X, file size %u", ExeName().c_str(),
                        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, fa.nFileSizeLow);
        }
        if (!configOk)
            dlog::Write("dmc1_practice.ini not found next to dinput8.dll; using defaults with no watches");
        else
            dlog::Write("Loaded config: %zu watches", g_config.watches.size());
        g_config.dataFolder = folder;

        // Heavy work happens on a separate thread; DllMain must stay minimal.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
