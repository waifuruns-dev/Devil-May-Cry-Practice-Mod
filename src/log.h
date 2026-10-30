#pragma once
#include <string>

namespace dlog {
void Open(const std::wstring& path);
void Write(const char* fmt, ...);
// Detailed tracing (event flags, room loads step by step). Only written with [Debug] VerboseLog = 1.
void Verbose(const char* fmt, ...);
extern bool g_verbose;
}  // namespace dlog
