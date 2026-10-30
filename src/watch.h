#pragma once
#include "config.h"

#include <cstdint>
#include <string>
#include <vector>

// A parsed address: a base (absolute, or module + offset) followed by
// optional pointer hops. Resolved fresh every read, so moving data still works.
struct AddressExpr {
    std::wstring module;            // empty = absolute address
    uintptr_t base = 0;
    std::vector<intptr_t> offsets;  // each hop: read pointer, then add offset
    bool valid = false;
};

bool ParseAddress(const std::string& text, AddressExpr& out);

// Follows the module base and pointer hops. False if any hop is unreadable.
bool ResolveAddress(const AddressExpr& addr, uintptr_t& out);

// Writes a 16-bit value. Used only by the HP/DT fix after a preset loads.
bool WriteU16(const AddressExpr& addr, uint16_t value);

// Returns false if any part of the read fails (bad pointer, unmapped memory).
bool ReadWatch(const AddressExpr& addr, ValueType type, double& value);

// Turns a value into display text according to its format.
std::string FormatValue(double value, ValueType type, Format format, double ticksPerSecond);
