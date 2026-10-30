#include "backup.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "log.h"

// DMC1's saves are in Steam's folder: Steam\userdata\<account>\631510\remote\ (dmc1.sav, options.sav).
namespace {

std::wstring SteamPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    std::wstring s = buf;
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

bool HasSaves(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\dmc1.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// Every account folder that has DmC saves.
std::vector<std::wstring> SaveFolders() {
    std::vector<std::wstring> out;
    const std::wstring steam = SteamPath();
    if (steam.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((steam + L"\\userdata\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring dir = steam + L"\\userdata\\" + fd.cFileName + L"\\631510\\remote";
        if (HasSaves(dir)) out.push_back(dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring AccountOf(const std::wstring& remote) {
    const size_t a = remote.find(L"\\userdata\\");
    if (a == std::wstring::npos) return L"unknown";
    const size_t b = remote.find(L'\\', a + 10);
    return remote.substr(a + 10, b == std::wstring::npos ? std::wstring::npos : b - a - 10);
}

// Keeps the newest `keep` backups of an account, deletes older ones.
void Prune(const std::wstring& dir, size_t keep) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());  // timestamps sort by age
    while (names.size() > keep) {
        const std::wstring old = dir + L"\\" + names.front();
        WIN32_FIND_DATAW f2;
        HANDLE h2 = FindFirstFileW((old + L"\\*").c_str(), &f2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((old + L"\\" + f2.cFileName).c_str());
            } while (FindNextFileW(h2, &f2));
            FindClose(h2);
        }
        RemoveDirectoryW(old.c_str());
        names.erase(names.begin());
    }
}

}  // namespace

void backup::BackUpSaves(const std::wstring& modFolder) {
    const std::vector<std::wstring> folders = SaveFolders();
    if (folders.empty()) {
        dlog::Write("[Backup] no DMC1 save found in Steam's folder");
        return;
    }
    time_t t = time(nullptr);
    tm l{};
    localtime_s(&l, &t);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d_%H-%M-%S", &l);
    const std::wstring root = modFolder + L"dmc1_save_backups";
    CreateDirectoryW(root.c_str(), nullptr);
    for (const std::wstring& remote : folders) {
        const std::wstring account = root + L"\\" + AccountOf(remote);
        CreateDirectoryW(account.c_str(), nullptr);
        const std::wstring dest = account + L"\\" + stamp;
        CreateDirectoryW(dest.c_str(), nullptr);
        int copied = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((remote + L"\\*.sav").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (CopyFileW((remote + L"\\" + fd.cFileName).c_str(), (dest + L"\\" + fd.cFileName).c_str(), FALSE))
                    ++copied;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        Prune(account, 20);
        char a[256], d[512];
        WideCharToMultiByte(CP_UTF8, 0, remote.c_str(), -1, a, sizeof(a), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, dest.c_str(), -1, d, sizeof(d), nullptr, nullptr);
        dlog::Write("[Backup] %d save files from %s -> %s", copied, a, d);
    }
}
