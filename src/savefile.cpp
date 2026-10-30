#include "savefile.h"
#include "log.h"

#include <windows.h>
#include <cstdio>

namespace {

constexpr size_t kSlotSize = 0x970;
constexpr size_t kFileSize = 10 * kSlotSize + 0x104;

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring SteamPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    std::wstring p(buf);
    for (wchar_t& c : p) if (c == L'/') c = L'\\';
    return p;
}

DWORD SteamActiveUser() {
    DWORD id = 0, size = sizeof(id);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, nullptr, &id, &size) != ERROR_SUCCESS)
        return 0;
    return id;
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    return ok;
}

}  // namespace

std::wstring savefile::Locate(const std::wstring& iniOverride) {
    if (!iniOverride.empty()) {
        dlog::Write("[Save] using SaveFile from INI: %ls (%s)", iniOverride.c_str(), Exists(iniOverride) ? "found" : "NOT FOUND");
        return iniOverride;
    }

    const std::wstring steam = SteamPath();
    if (steam.empty()) {
        dlog::Write("[Save] Steam path not found in the registry; set SaveFile in the INI");
        return L"";
    }

    // 1) The account Steam says is logged in.
    if (DWORD id = SteamActiveUser()) {
        std::wstring p = steam + L"\\userdata\\" + std::to_wstring(id) + L"\\631510\\remote\\dmc1.sav";
        if (Exists(p)) { dlog::Write("[Save] found %ls", p.c_str()); return p; }
    }

    // 2) Otherwise, the most recently changed dmc1.sav under any account.
    std::wstring best;
    FILETIME bestTime = {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((steam + L"\\userdata\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring p = steam + L"\\userdata\\" + fd.cFileName + L"\\631510\\remote\\dmc1.sav";
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &bestTime) > 0) {
                best = p;
                bestTime = a.ftLastWriteTime;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty()) dlog::Write("[Save] no dmc1.sav found under %ls\\userdata; set SaveFile in the INI", steam.c_str());
    else dlog::Write("[Save] found %ls", best.c_str());
    return best;
}

namespace {
// The game reads dmc1.sav into memory once at startup (10 slots of 0x970 bytes from 0x139DB0) and
// Load Game reads that copy, so a slot written to the file mid-session only shows up after a restart.
// Write the in-memory copy too. Only done if it looks right (slot 1 has a valid checksum).
constexpr uintptr_t kMemSaveFile = 0x139DB0;
bool SlotChecksumOk(const uint8_t* s) {
    uint32_t sum = 0;
    for (size_t i = 0x0A; i < kSlotSize; ++i) sum += s[i];
    return (uint16_t)(sum & 0xFFFF) == (uint16_t)(s[8] | (s[9] << 8)) && (sum & 0xFFFF) != 0;
}
}  // namespace

bool savefile::WriteSlotToMemory(int slot, const std::vector<uint8_t>& data) {
    if (slot < 1 || slot > 10 || data.size() != kSlotSize) return false;
    std::vector<uint8_t> first(kSlotSize);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(kMemSaveFile), first.data(), kSlotSize, &got) ||
        got != kSlotSize)
        return false;
    // Any of the 10 slots with a valid checksum confirms the layout (slot 1 may be empty).
    bool ok = false;
    for (int i = 0; i < 10 && !ok; ++i) {
        std::vector<uint8_t> sl(kSlotSize);
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(kMemSaveFile + i * kSlotSize), sl.data(),
                              kSlotSize, &got) && got == kSlotSize && SlotChecksumOk(sl.data()))
            ok = true;
    }
    if (!ok) { dlog::Write("[Save] in-memory save file not found; slot written to disk only"); return false; }
    SIZE_T done = 0;
    const bool w = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(kMemSaveFile + (slot - 1) * kSlotSize),
                                      data.data(), kSlotSize, &done) && done == kSlotSize;
    if (w) dlog::Write("[Save] slot %d updated in the game's memory too (Load Game sees it now)", slot);
    return w;
}

bool savefile::ReadSlotsFromMemory(std::vector<uint8_t>& out) {
    out.assign(10 * kSlotSize, 0);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(kMemSaveFile), out.data(), out.size(), &got) ||
        got != out.size())
        return false;
    for (int i = 0; i < 10; ++i)
        if (SlotChecksumOk(out.data() + i * kSlotSize)) return true;
    return false;  // no valid slot: not the save copy
}

bool savefile::WriteSlotsToMemory(const std::vector<uint8_t>& data) {
    if (data.size() != 10 * kSlotSize) return false;
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(kMemSaveFile), data.data(), data.size(),
                              &done) && done == data.size();
}

bool savefile::ReadSlotsFromFile(const std::wstring& path, std::vector<uint8_t>& out) {
    std::vector<uint8_t> file;
    if (!ReadAll(path, file) || file.size() != kFileSize) return false;
    out.assign(file.begin(), file.begin() + 10 * kSlotSize);
    return true;
}

bool savefile::CopyWhole(const std::wstring& from, const std::wstring& to) {
    std::vector<uint8_t> data;
    return ReadAll(from, data) && !data.empty() && WriteAll(to, data);
}

bool savefile::WriteSlot(const std::wstring& path, int slot, const std::vector<uint8_t>& data, std::string& error) {
    if (path.empty()) { error = "save file not found (set SaveFile in the INI)"; return false; }
    if (slot < 1 || slot > 10 || data.size() != kSlotSize) { error = "bad slot"; return false; }

    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) { error = "couldn't read the save file"; return false; }
    if (file.size() != kFileSize) { error = "save file has an unexpected size; not touching it"; return false; }

    // One-time backup of the file as it was before the mod ever wrote to it.
    const std::wstring backup = path + L".practice_backup";
    if (!Exists(backup)) {
        if (!WriteAll(backup, file)) { error = "couldn't create the backup; not touching the save"; return false; }
        dlog::Write("[Save] backup written: %ls", backup.c_str());
    }

    std::copy(data.begin(), data.end(), file.begin() + (slot - 1) * kSlotSize);

    // Write to a temp file, then swap it in, so a crash mid-write can't corrupt the save.
    const std::wstring tmp = path + L".practice_tmp";
    if (!WriteAll(tmp, file)) { error = "couldn't write the save file"; return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        error = "couldn't replace the save file (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}
