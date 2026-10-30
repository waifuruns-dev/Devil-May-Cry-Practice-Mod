#include "watch.h"
#include "game_addrs.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

bool ParseHex(const std::string& s, uintptr_t& out) {
    std::string t = Trim(s);
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty()) return false;
    try {
        size_t used = 0;
        out = (uintptr_t)std::stoull(t, &used, 16);
        return used == t.size();
    } catch (...) {
        return false;
    }
}

// Safe read: ReadProcessMemory on our own process fails cleanly instead of
// crashing the game when an address is unmapped (e.g. a boss that isn't loaded).
bool SafeRead(uintptr_t addr, void* dst, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, size, &got) && got == size;
}

size_t SizeOf(ValueType t) {
    switch (t) {
        case ValueType::U8: case ValueType::I8: return 1;
        case ValueType::U16: case ValueType::I16: return 2;
        default: return 4;
    }
}

}  // namespace

bool ParseAddress(const std::string& text, AddressExpr& out) {
    out = AddressExpr{};

    // Split on "->" into base + hops.
    std::vector<std::string> parts;
    std::string rest = text;
    for (;;) {
        size_t arrow = rest.find("->");
        parts.push_back(Trim(rest.substr(0, arrow)));
        if (arrow == std::string::npos) break;
        rest = rest.substr(arrow + 2);
    }

    // Base: "module.exe+OFFSET" or plain hex.
    const std::string& base = parts[0];
    size_t plus = base.find('+');
    if (!base.empty() && base[0] == '@') {
        // "@StartFrame", "@Session" ...: an exe address found at startup (game_addrs).
        if (!game_addrs::Lookup(base.substr(1), out.base)) return false;
        out.module = L"*";  // the game's exe
    } else if (plus != std::string::npos) {
        std::string mod = Trim(base.substr(0, plus));
        if (mod.empty() || !ParseHex(base.substr(plus + 1), out.base)) return false;
        out.module.assign(mod.begin(), mod.end());
    } else if (!ParseHex(base, out.base)) {
        return false;
    }

    for (size_t i = 1; i < parts.size(); ++i) {
        std::string p = parts[i];
        bool negative = !p.empty() && p[0] == '-';
        if (negative) p = p.substr(1);
        uintptr_t off;
        if (!ParseHex(p, off)) return false;
        out.offsets.push_back(negative ? -(intptr_t)off : (intptr_t)off);
    }

    out.valid = true;
    return true;
}

bool ResolveAddress(const AddressExpr& a, uintptr_t& out) {
    if (!a.valid) return false;

    uintptr_t addr = a.base;
    if (!a.module.empty()) {
        HMODULE mod = a.module == L"*" ? GetModuleHandleW(nullptr) : GetModuleHandleW(a.module.c_str());
        if (!mod) return false;
        addr += reinterpret_cast<uintptr_t>(mod);
    }

    for (intptr_t off : a.offsets) {
        uintptr_t ptr = 0;
        if (!SafeRead(addr, &ptr, sizeof(ptr)) || ptr == 0) return false;
        addr = ptr + off;
    }
    out = addr;
    return true;
}

bool WriteU16(const AddressExpr& a, uint16_t value) {
    uintptr_t addr;
    if (!ResolveAddress(a, addr)) return false;
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), &value, sizeof(value), &done) &&
           done == sizeof(value);
}

bool ReadWatch(const AddressExpr& a, ValueType type, double& value) {
    uintptr_t addr;
    if (!ResolveAddress(a, addr)) return false;

    unsigned char buf[4] = {};
    if (!SafeRead(addr, buf, SizeOf(type))) return false;

    switch (type) {
        case ValueType::U8:  value = *reinterpret_cast<uint8_t*>(buf);  break;
        case ValueType::I8:  value = *reinterpret_cast<int8_t*>(buf);   break;
        case ValueType::U16: value = *reinterpret_cast<uint16_t*>(buf); break;
        case ValueType::I16: value = *reinterpret_cast<int16_t*>(buf);  break;
        case ValueType::U32: value = *reinterpret_cast<uint32_t*>(buf); break;
        case ValueType::I32: value = *reinterpret_cast<int32_t*>(buf);  break;
        case ValueType::F32: value = *reinterpret_cast<float*>(buf);    break;
        case ValueType::Text: return false;  // text rows are never read from memory
    }
    return true;
}

std::string FormatValue(double v, ValueType type, Format format, double tps) {
    char buf[64];
    switch (format) {
        case Format::Time: {
            if (tps <= 0) tps = 60.0;
            double seconds = v / tps;
            if (seconds < 0) seconds = 0;
            long long totalCs = (long long)std::floor(seconds * 100.0);
            long long cs = totalCs % 100;
            long long s = (totalCs / 100) % 60;
            long long m = (totalCs / 6000) % 60;
            long long h = totalCs / 360000;
            if (h > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", h, m, s, cs);
            else       snprintf(buf, sizeof(buf), "%lld:%02lld.%02lld", m, s, cs);
            break;
        }
        case Format::Float:
            snprintf(buf, sizeof(buf), "%.3f", v);
            break;
        case Format::Hex:
            snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)(long long)v);
            break;
        case Format::Int:
        default:
            if (type == ValueType::F32) snprintf(buf, sizeof(buf), "%.3f", v);
            else                        snprintf(buf, sizeof(buf), "%lld", (long long)v);
            break;
    }
    return buf;
}
