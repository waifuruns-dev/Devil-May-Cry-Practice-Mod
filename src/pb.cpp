#include "pb.h"
#include "log.h"

#include <cstdio>
#include <map>
#include <sstream>
#include <utility>

namespace {

using Key = std::pair<std::string, int>;  // (category, mission)
std::map<Key, double> g_bests;
std::wstring g_path;

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void Save() {
    if (g_path.empty()) return;
    FILE* f = _wfopen(g_path.c_str(), L"w");
    if (!f) {
        dlog::Write("[PB] couldn't write the personal best file");
        return;
    }
    fprintf(f, "; DMC1 Practice personal bests: Category | Mission | Frames (60 per second)\n");
    fprintf(f, "; Delete a line to clear that PB. Edit with the game closed.\n");
    for (const auto& kv : g_bests)
        fprintf(f, "%s | %d | %.0f\n", kv.first.first.c_str(), kv.first.second, kv.second);
    fclose(f);
}

}  // namespace

void pb::Load(const std::wstring& path) {
    g_path = path;
    g_bests.clear();

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;  // no PBs yet
    std::string contents;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) contents.append(chunk, n);
    fclose(f);

    std::istringstream in(contents);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        size_t a = line.find('|');
        size_t b = line.find('|', a == std::string::npos ? a : a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        try {
            std::string cat = Trim(line.substr(0, a));
            int mission = std::stoi(Trim(line.substr(a + 1, b - a - 1)));
            double frames = std::stod(Trim(line.substr(b + 1)));
            g_bests[{cat, mission}] = frames;
        } catch (...) {
        }
    }
    dlog::Write("[PB] loaded %zu personal bests", g_bests.size());
}

bool pb::Get(const std::string& category, int mission, double& frames) {
    auto it = g_bests.find({category, mission});
    if (it == g_bests.end()) return false;
    frames = it->second;
    return true;
}

bool pb::Submit(const std::string& category, int mission, double frames, double& previous) {
    previous = -1;
    auto it = g_bests.find({category, mission});
    if (it != g_bests.end()) previous = it->second;

    if (previous >= 0 && frames >= previous) return false;
    g_bests[{category, mission}] = frames;
    Save();
    dlog::Write("[PB] new best: %s mission %d = %.0f frames", category.c_str(), mission, frames);
    return true;
}
