#include "dump.h"
#include "config.h"
#include "log.h"
#include "game_addrs.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

namespace {

const uintptr_t& kSessionPtr = g_addrs.session;
const uintptr_t& kStagePtr = g_addrs.stage;
const uintptr_t& kActorPtr = g_addrs.actor;

bool g_keyDown = false;
int g_next = 1;
ULONGLONG g_msgUntil = 0;
char g_msg[256] = {};

bool Read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
uintptr_t Deref(uintptr_t moduleOffset) {
    if (!moduleOffset) return 0;  // not found for this game build
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t p = 0;
    return Read(base + moduleOffset, &p, sizeof(p)) ? p : 0;
}
uint32_t ReadU32(uintptr_t addr) {
    uint32_t v = 0;
    Read(addr, &v, 4);
    return v;
}

bool Writable(DWORD protect) {
    protect &= 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

struct Region { uintptr_t base; size_t size; std::string what; };

// Committed, writable memory in [start, end).
void Collect(uintptr_t start, uintptr_t end, const char* what, std::vector<Region>& out) {
    uintptr_t p = start;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) {
        uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t re = rb + mbi.RegionSize;
        if (re > end) re = end;
        if (rb < start) rb = start;
        if (mbi.State == MEM_COMMIT && Writable(mbi.Protect) && !(mbi.Protect & PAGE_GUARD))
            out.push_back({rb, re - rb, what});
        if (re <= p) break;
        p = re;
    }
}

// The whole allocation that holds addr (the game's fixed data block at 0x100000.. is one).
void CollectAllocation(uintptr_t addr, const char* what, std::vector<Region>& out) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
    uintptr_t allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    uintptr_t p = allocBase, end = allocBase;
    while (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)) &&
           reinterpret_cast<uintptr_t>(mbi.AllocationBase) == allocBase) {
        end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        p = end;
    }
    for (const Region& r : out)  // already covered
        if (allocBase >= r.base && allocBase < r.base + r.size) return;
    Collect(allocBase, end, what, out);
}

void DoDump() {
    const int n = g_next++;
    wchar_t dirName[32];
    swprintf(dirName, 32, L"dumps\\%03d\\", n);
    const std::wstring root = g_config.dataFolder + L"dumps\\";
    const std::wstring dir = g_config.dataFolder + dirName;
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    const uintptr_t session = Deref(kSessionPtr), stage = Deref(kStagePtr), actor = Deref(kActorPtr);

    std::vector<Region> regions;
    Collect(exe, exe + nt->OptionalHeader.SizeOfImage, "dmc1.exe data", regions);
    if (session) CollectAllocation(session, "session block", regions);
    if (stage) CollectAllocation(stage, "stage manager", regions);
    if (actor) CollectAllocation(actor, "player", regions);

    // The game's code and read-only data (every non-writable section of dmc1.exe), so the logic can
    // be read offline and addresses re-found on other game versions. One file per section:
    // section_<name>_<rva>.bin
    {
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) continue;  // writable data is dumped above
            const size_t size = sec->Misc.VirtualSize;
            std::vector<uint8_t> buf(size);
            for (size_t off = 0; off < size; off += 0x1000) {
                const size_t len = size - off < 0x1000 ? size - off : 0x1000;
                if (!Read(exe + sec->VirtualAddress + off, buf.data() + off, len)) memset(buf.data() + off, 0, len);
            }
            char nm[9] = {};
            memcpy(nm, sec->Name, 8);
            for (char& c : nm) if (c && !isalnum((unsigned char)c)) c = '_';
            wchar_t fn[64];
            swprintf(fn, 64, L"section_%hs_%06X.bin", nm, sec->VirtualAddress);
            if (FILE* f = _wfopen((dir + fn).c_str(), L"wb")) {
                fwrite(buf.data(), 1, buf.size(), f);
                fclose(f);
            }
        }
    }

    FILE* info = _wfopen((dir + L"info.txt").c_str(), L"w");
    if (info) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(info, "dump %d at %02d:%02d:%02d\n", n, t.wHour, t.wMinute, t.wSecond);
        fprintf(info, "dmc1.exe base %llX\n", (unsigned long long)exe);
        fprintf(info, "session %llX  stage %llX  player %llX\n", (unsigned long long)session,
                (unsigned long long)stage, (unsigned long long)actor);
        if (session) fprintf(info, "events %08X\n", ReadU32(session + 0x1CA4));
        if (stage)
            fprintf(info, "track %u room %u  next track %u room %u\n", ReadU32(stage + 0x270),
                    ReadU32(stage + 0x274), ReadU32(stage + 0x288), ReadU32(stage + 0x28C));
        fprintf(info, "\nregions (file = address in hex):\n");
    }

    size_t total = 0;
    std::vector<uint8_t> page(0x1000);
    for (const Region& r : regions) {
        wchar_t name[48];
        swprintf(name, 48, L"%012llX.bin", (unsigned long long)r.base);
        FILE* f = _wfopen((dir + name).c_str(), L"wb");
        if (!f) continue;
        for (size_t off = 0; off < r.size; off += page.size()) {
            const size_t len = (r.size - off < page.size()) ? r.size - off : page.size();
            if (!Read(r.base + off, page.data(), len)) memset(page.data(), 0, len);
            fwrite(page.data(), 1, len, f);
        }
        fclose(f);
        total += r.size;
        if (info) fprintf(info, "%012llX  size %8zX  %s\n", (unsigned long long)r.base, r.size, r.what.c_str());
    }
    if (info) fclose(info);

    dlog::Write("[Dump] dump %d saved: %zu regions, %.1f MB", n, regions.size(), total / 1048576.0);
    snprintf(g_msg, sizeof(g_msg), "Dump %d saved (%.1f MB)", n, total / 1048576.0);
    g_msgUntil = GetTickCount64() + 2500;
}

}  // namespace

void dump::Init() {
    // Continue numbering after existing dumps so nothing gets overwritten.
    while (true) {
        wchar_t dirName[32];
        swprintf(dirName, 32, L"dumps\\%03d", g_next);
        if (GetFileAttributesW((g_config.dataFolder + dirName).c_str()) == INVALID_FILE_ATTRIBUTES) break;
        ++g_next;
    }
}

void dump::Update(bool focused) {
    if (!g_config.dumpKey) return;
    const bool down = focused && (GetAsyncKeyState(g_config.dumpKey) & 0x8000) != 0;
    if (down && !g_keyDown) DoDump();
    g_keyDown = down;
}

void dump::ShowMessage(const char* text, unsigned ms) {
    snprintf(g_msg, sizeof(g_msg), "%s", text);
    g_msgUntil = GetTickCount64() + ms;
}

bool dump::WantsDraw() { return GetTickCount64() < g_msgUntil; }

void dump::Draw() {
    if (!WantsDraw()) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.75f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##dumpmsg", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextUnformatted(g_msg);
    ImGui::End();
}
