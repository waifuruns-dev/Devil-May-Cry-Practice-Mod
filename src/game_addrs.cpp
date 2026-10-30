#include "game_addrs.h"
#include "log.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

GameAddrs g_addrs;

namespace {

bool g_quiet = false;  // retries: don't log every miss

// Pattern tokens: hex byte, "??" any byte, "D" a 4-byte relative field to capture (rip-relative
// data or a call target: target = field address + 4 + value), "[" ... "]" an optional group.
struct Tok { enum Kind { Byte, Any, Cap, OptBegin, OptEnd } kind; uint8_t value = 0; int match = -1; };

std::vector<Tok> Compile(const char* pattern) {
    std::vector<Tok> out;
    std::vector<int> stack;
    const char* p = pattern;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        const std::string tok(start, p);  // whole space-separated token, so "DA" is a byte, "D" a capture
        if (tok == "[") { stack.push_back((int)out.size()); out.push_back({Tok::OptBegin}); }
        else if (tok == "]") {
            const int open = stack.back();
            stack.pop_back();
            out[open].match = (int)out.size();
            out.push_back({Tok::OptEnd});
        }
        else if (tok == "??") out.push_back({Tok::Any});
        else if (tok == "D") out.push_back({Tok::Cap});
        else out.push_back({Tok::Byte, (uint8_t)strtoul(tok.c_str(), nullptr, 16)});
    }
    return out;
}

// Backtracking match at `pos`. Records capture positions.
bool MatchAt(const uint8_t* data, size_t size, size_t pos, const std::vector<Tok>& t, size_t ti,
             std::vector<size_t>& caps) {
    while (ti < t.size()) {
        const Tok& k = t[ti];
        switch (k.kind) {
            case Tok::Byte:
                if (pos >= size || data[pos] != k.value) return false;
                ++pos; ++ti; break;
            case Tok::Any:
                if (pos >= size) return false;
                ++pos; ++ti; break;
            case Tok::Cap:
                if (pos + 4 > size) return false;
                caps.push_back(pos);
                pos += 4; ++ti; break;
            case Tok::OptBegin: {
                const size_t mark = caps.size();
                if (MatchAt(data, size, pos, t, ti + 1, caps)) return true;  // with the group
                caps.resize(mark);
                ti = k.match + 1;  // without it
                break;
            }
            case Tok::OptEnd: ++ti; break;
        }
    }
    return true;
}

struct Text { const uint8_t* data = nullptr; size_t size = 0; uintptr_t rva = 0; };

Text CodeSection() {
    Text t;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            t.data = reinterpret_cast<const uint8_t*>(base + sec->VirtualAddress);
            t.size = sec->Misc.VirtualSize;
            t.rva = sec->VirtualAddress;
        }
    }
    return t;
}

// Finds the pattern; it must occur exactly once. Returns the targets of its captures as offsets
// from the exe base.
bool Find(const Text& text, const char* name, const char* pattern, std::vector<uintptr_t>& targets,
          bool quiet = false) {
    const std::vector<Tok> toks = Compile(pattern);
    const uint8_t first = toks[0].value;  // every pattern here starts with a fixed byte
    int hits = 0;
    std::vector<size_t> caps, found;
    for (size_t pos = 0; pos < text.size; ++pos) {
        if (text.data[pos] != first) continue;
        caps.clear();
        if (MatchAt(text.data, text.size, pos, toks, 0, caps)) {
            if (++hits == 1) found = caps;
        }
    }
    if (hits != 1) {
        if ((quiet && hits == 0) || g_quiet) return false;  // another variant follows / a retry
        dlog::Write("[Addrs] %s: pattern matched %d times (needs exactly 1); not found", name, hits);
        return false;
    }
    targets.clear();
    for (size_t c : found) {
        int32_t rel;
        memcpy(&rel, text.data + c, 4);
        targets.push_back(text.rva + c + 4 + rel);
    }
    return true;
}

}  // namespace

bool game_addrs::Resolve(bool quiet) {
    g_quiet = quiet;
    const Text text = CodeSection();
    if (!text.data) { dlog::Write("[Addrs] no .text section in dmc1.exe?"); return false; }
    g_addrs = GameAddrs();
    std::vector<uintptr_t> t;

    // Mission 7+1 -> 8 fix-up: mov rdx,[session] ; mov rcx,[stage] ; (mov rbx,rdx) ; cmp byte [rdx+1E65],1
    if (Find(text, "session/stage", "48 8B 15 D 48 8B 0D D [ 48 8B DA ] 80 BA 65 1E 00 00 01", t)) {
        g_addrs.session = t[0];
        g_addrs.stage = t[1];
    }
    // Re-equip after a load: actor pointer, then form / gun / melee compared and switched in turn.
    if (Find(text, "player/weapons",
             "48 8B 05 D 0F B6 0D ?? ?? ?? ?? 38 88 4C 1C 00 00 74 ?? [ 0F B6 C9 ] E8 D "
             "48 8B 05 ?? ?? ?? ?? 0F B6 0D ?? ?? ?? ?? 38 88 48 1C 00 00 74 ?? [ 0F B6 C9 ] E8 D "
             "48 8B 05 ?? ?? ?? ?? 0F B6 0D ?? ?? ?? ?? 38 88 CC 1B 00 00 74 ?? [ 0F B6 C9 ] 33 D2 E8 D",
             t)) {
        g_addrs.actor = t[0];
        g_addrs.fnForm = t[1];
        g_addrs.fnGun = t[2];
        g_addrs.fnMelee = t[3];
    }
    // Mission start: mov eax,[rdi+0xBC] (frame counter) ... mov [startFrame],eax. The instruction in
    // between differs per build.
    if (Find(text, "start frame", "8B 87 BC 00 00 00 C6 87 65 1E 00 00 00 89 05 D", t, true) ||  // 2018 build
        Find(text, "start frame", "8B 87 BC 00 00 00 8B 8F 84 26 00 00 89 05 D", t))               // newer build
        g_addrs.startFrame = t[0];
    // Mission start spawn: shl rax,5 ; movups xmm0,[rax+r8+10] ; movaps [nextPos],xmm0
    if (Find(text, "spawn position", "48 C1 E0 05 42 0F 10 44 00 10 0F 29 05 D", t)) g_addrs.nextPos = t[0];

    // Save buffer: mov rbx,[saveBuf] ; lea rdx,[rbx+70] ; call (the startup read of dmc1.sav)
    if (Find(text, "save buffer", "48 8B 1D D 48 8D 53 70 E8", t)) g_addrs.saveBuf = t[0];
    // HUD: mov rax,[hud] ; movzx ecx,byte [rcx+2464] ; mov [rax+98],cl (HP bars after a blue orb)
    if (Find(text, "HUD", "48 8B 05 D ?? ?? ?? ?? ?? ?? ?? 88 88 98 00 00 00", t)) g_addrs.hud = t[0];

    const bool all = g_addrs.session && g_addrs.stage && g_addrs.actor && g_addrs.startFrame && g_addrs.nextPos &&
                     g_addrs.fnForm && g_addrs.fnGun && g_addrs.fnMelee && g_addrs.saveBuf && g_addrs.hud;
    if (quiet && !all) return false;
    dlog::Write("[Addrs] session +%llX, stage +%llX, player +%llX, start frame +%llX, spawn +%llX",
                (unsigned long long)g_addrs.session, (unsigned long long)g_addrs.stage,
                (unsigned long long)g_addrs.actor, (unsigned long long)g_addrs.startFrame,
                (unsigned long long)g_addrs.nextPos);
    dlog::Write("[Addrs] save buffer +%llX, HUD +%llX", (unsigned long long)g_addrs.saveBuf,
                (unsigned long long)g_addrs.hud);
    dlog::Write("[Addrs] weapon functions: form +%llX, gun +%llX, melee +%llX", (unsigned long long)g_addrs.fnForm,
                (unsigned long long)g_addrs.fnGun, (unsigned long long)g_addrs.fnMelee);
    return all;
}

bool game_addrs::Lookup(const std::string& name, uintptr_t& offset) {
    std::string n;
    for (char c : name) n += (char)tolower((unsigned char)c);
    if (n == "session") offset = g_addrs.session;
    else if (n == "stage") offset = g_addrs.stage;
    else if (n == "actor" || n == "player") offset = g_addrs.actor;
    else if (n == "startframe") offset = g_addrs.startFrame;
    else if (n == "nextpos") offset = g_addrs.nextPos;
    else if (n == "hud") offset = g_addrs.hud;
    else if (n == "savebuf") offset = g_addrs.saveBuf;
    else return false;
    return offset != 0;
}

void game_addrs::ResolveWithRetry(unsigned timeoutMs) {
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    int tries = 0;
    while (GetTickCount64() < until) {
        ++tries;
        if (Resolve(true)) {
            if (tries > 1) dlog::Write("[Addrs] found everything after %d tries (code was still encrypted at launch)", tries);
            return;
        }
        Sleep(250);
    }
    dlog::Write("[Addrs] still missing addresses after %u s; details:", timeoutMs / 1000);
    Resolve(false);  // final attempt, logs which ones are missing
}
