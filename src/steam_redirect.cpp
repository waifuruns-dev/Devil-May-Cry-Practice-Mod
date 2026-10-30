#include "steam_redirect.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>

#include "log.h"

namespace {

using SteamAPICall_t = uint64_t;
using UGCFileWriteStreamHandle_t = uint64_t;

// ISteamRemoteStorage (STEAMREMOTESTORAGE_INTERFACE_VERSION016) vtable slots.
enum {
    kFileWrite = 0,
    kFileRead = 1,
    kFileWriteAsync = 2,
    kFileReadAsync = 3,
    kFileWriteStreamOpen = 9,
    kFileWriteStreamWriteChunk = 10,
    kFileWriteStreamClose = 11,
    kFileWriteStreamCancel = 12,
};

using FileWriteFn = bool (*)(void*, const char*, const void*, int32_t);
using FileReadFn = int32_t (*)(void*, const char*, void*, int32_t);
using FileWriteAsyncFn = SteamAPICall_t (*)(void*, const char*, const void*, uint32_t);
using FileReadAsyncFn = SteamAPICall_t (*)(void*, const char*, uint32_t, uint32_t);
using StreamOpenFn = UGCFileWriteStreamHandle_t (*)(void*, const char*);
using StreamChunkFn = bool (*)(void*, UGCFileWriteStreamHandle_t, const void*, int32_t);
using StreamCloseFn = bool (*)(void*, UGCFileWriteStreamHandle_t);

FileWriteFn g_origWrite = nullptr;
FileReadFn g_origRead = nullptr;
FileWriteAsyncFn g_origWriteAsync = nullptr;
FileReadAsyncFn g_origReadAsync = nullptr;
StreamOpenFn g_origStreamOpen = nullptr;
StreamChunkFn g_origStreamChunk = nullptr;
StreamCloseFn g_origStreamClose = nullptr;
StreamCloseFn g_origStreamCancel = nullptr;

std::atomic<bool> g_active{false}, g_installed{false};
void* g_rs = nullptr;  // ISteamRemoteStorage
std::wstring g_practiceFile;
std::mutex g_mu;
std::set<UGCFileWriteStreamHandle_t> g_redirectedStreams;
std::vector<uint8_t> g_streamData;
int g_logged = 0;

bool IsSave(const char* name) { return name && _stricmp(name, "dmc1.sav") == 0; }

void Note(const char* what, const char* name, long long size) {
    if (g_logged++ < 200) dlog::Write("[Steam] %s %s (%lld bytes)%s", what, name ? name : "?", size,
                                      g_active ? " -> practice file" : "");
}

bool WritePractice(const void* data, size_t size) {
    const std::wstring tmp = g_practiceFile + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0 || !ok) return false;
    return MoveFileExW(tmp.c_str(), g_practiceFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Steam's current copy of the real save (to answer an async write without changing it).
std::vector<uint8_t> RealSave(void* self) {
    std::vector<uint8_t> buf(1 << 20);
    const int32_t n = g_origRead ? g_origRead(self, "dmc1.sav", buf.data(), (int32_t)buf.size()) : 0;
    buf.resize(n > 0 ? n : 0);
    return buf;
}

bool HookedWrite(void* self, const char* name, const void* data, int32_t size) {
    if (IsSave(name)) Note("FileWrite", name, size);
    if (g_active && IsSave(name)) return WritePractice(data, (size_t)size);
    return g_origWrite(self, name, data, size);
}

// Load Game reads the file again each time: while redirected it gets the practice file.
int32_t ReadPractice(void* data, int32_t size) {
    FILE* f = _wfopen(g_practiceFile.c_str(), L"rb");
    if (!f) return 0;
    const size_t n = fread(data, 1, (size_t)size, f);
    fclose(f);
    return (int32_t)n;
}

int32_t HookedRead(void* self, const char* name, void* data, int32_t size) {
    if (g_active && IsSave(name)) {
        const int32_t n = ReadPractice(data, size);
        Note("FileRead", name, n);
        if (n > 0) return n;
        dlog::Write("[Steam] practice file unreadable: the game reads your real save instead");
    }
    const int32_t n = g_origRead(self, name, data, size);
    if (IsSave(name) && !g_active) Note("FileRead", name, n);
    return n;
}

SteamAPICall_t HookedWriteAsync(void* self, const char* name, const void* data, uint32_t size) {
    if (IsSave(name)) Note("FileWriteAsync", name, size);
    if (g_active && IsSave(name)) {
        WritePractice(data, size);
        // The game waits for Steam's "write done" callback: write the real save back unchanged.
        const std::vector<uint8_t> real = RealSave(self);
        if (!real.empty()) return g_origWriteAsync(self, name, real.data(), (uint32_t)real.size());
        dlog::Write("[Steam] couldn't read the real save to answer an async write; skipped it");
        return 0;
    }
    return g_origWriteAsync(self, name, data, size);
}

SteamAPICall_t HookedReadAsync(void* self, const char* name, uint32_t offset, uint32_t size) {
    if (IsSave(name)) Note("FileReadAsync", name, size);
    return g_origReadAsync(self, name, offset, size);
}

UGCFileWriteStreamHandle_t HookedStreamOpen(void* self, const char* name) {
    const UGCFileWriteStreamHandle_t h = g_origStreamOpen(self, name);
    if (IsSave(name)) {
        Note("FileWriteStreamOpen", name, 0);
        if (g_active) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_redirectedStreams.insert(h);
            g_streamData.clear();
        }
    }
    return h;
}

bool Redirected(UGCFileWriteStreamHandle_t h) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_redirectedStreams.count(h) != 0;
}

bool HookedStreamChunk(void* self, UGCFileWriteStreamHandle_t h, const void* data, int32_t size) {
    if (Redirected(h)) {
        std::lock_guard<std::mutex> lock(g_mu);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        g_streamData.insert(g_streamData.end(), p, p + size);
        return true;
    }
    return g_origStreamChunk(self, h, data, size);
}

bool HookedStreamClose(void* self, UGCFileWriteStreamHandle_t h) {
    if (Redirected(h)) {
        std::vector<uint8_t> data;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            data.swap(g_streamData);
            g_redirectedStreams.erase(h);
        }
        Note("FileWriteStreamClose", "dmc1.sav", (long long)data.size());
        WritePractice(data.data(), data.size());
        g_origStreamCancel(self, h);  // nothing reaches the real save
        return true;
    }
    return g_origStreamClose(self, h);
}

void* RemoteStorage() {
    HMODULE api = GetModuleHandleW(L"steam_api64.dll");
    if (!api) return nullptr;
    using Getter = void* (*)();
    for (const char* fn : {"SteamAPI_SteamRemoteStorage_v016", "SteamRemoteStorage"})
        if (auto g = reinterpret_cast<Getter>(GetProcAddress(api, fn)))
            if (void* p = g()) return p;
    // Older SDKs: SteamInternal_FindOrCreateUserInterface(user, version).
    using GetUser = int32_t (*)();
    using Find = void* (*)(int32_t, const char*);
    auto user = reinterpret_cast<GetUser>(GetProcAddress(api, "SteamAPI_GetHSteamUser"));
    auto find = reinterpret_cast<Find>(GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface"));
    if (user && find) return find(user(), "STEAMREMOTESTORAGE_INTERFACE_VERSION016");
    return nullptr;
}

bool Hook(void** vt, int slot, void* detour, void** orig) {
    return MH_CreateHook(vt[slot], detour, orig) == MH_OK && MH_EnableHook(vt[slot]) == MH_OK;
}

DWORD WINAPI InstallThread(LPVOID) {
    void* rs = nullptr;
    for (int i = 0; i < 600 && !(rs = RemoteStorage()); ++i) Sleep(100);
    if (!rs) {
        dlog::Write("[Steam] remote storage not found: the practice save can't be kept apart (the mod stays off)");
        return 0;
    }
    void** vt = *reinterpret_cast<void***>(rs);
    const bool ok = Hook(vt, kFileWrite, (void*)&HookedWrite, (void**)&g_origWrite) &&
                    Hook(vt, kFileRead, (void*)&HookedRead, (void**)&g_origRead) &&
                    Hook(vt, kFileWriteAsync, (void*)&HookedWriteAsync, (void**)&g_origWriteAsync) &&
                    Hook(vt, kFileReadAsync, (void*)&HookedReadAsync, (void**)&g_origReadAsync) &&
                    Hook(vt, kFileWriteStreamOpen, (void*)&HookedStreamOpen, (void**)&g_origStreamOpen) &&
                    Hook(vt, kFileWriteStreamWriteChunk, (void*)&HookedStreamChunk, (void**)&g_origStreamChunk) &&
                    Hook(vt, kFileWriteStreamClose, (void*)&HookedStreamClose, (void**)&g_origStreamClose);
    g_origStreamCancel = reinterpret_cast<StreamCloseFn>(vt[kFileWriteStreamCancel]);
    g_installed = ok;
    g_rs = rs;
    dlog::Write("[Steam] save redirect %s (remote storage at %p)", ok ? "ready" : "FAILED", rs);
    return 0;
}

}  // namespace

void steam_redirect::Install(const std::wstring& practiceFile) {
    g_practiceFile = practiceFile;
    MH_Initialize();  // here, not in the thread: the D3D hooks are installed right after this
    HANDLE t = CreateThread(nullptr, 0, &InstallThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void steam_redirect::SetActive(bool on) {
    g_active = on;
    dlog::Write("[Steam] dmc1.sav writes go to %s", on ? "the practice file" : "your real save");
}
bool steam_redirect::Active() { return g_active; }

bool steam_redirect::ReadRealSave(std::vector<uint8_t>& out) {
    if (!g_installed || !g_rs) return false;
    out = RealSave(g_rs);
    return !out.empty();
}

bool steam_redirect::WriteRealSave(const std::vector<uint8_t>& data) {
    if (!g_installed || !g_rs || data.empty()) return false;
    const bool ok = g_origWrite(g_rs, "dmc1.sav", data.data(), (int32_t)data.size());
    dlog::Write("[Steam] export: real dmc1.sav written (%zu bytes): %s", data.size(), ok ? "ok" : "FAILED");
    return ok;
}
bool steam_redirect::Installed() { return g_installed; }
