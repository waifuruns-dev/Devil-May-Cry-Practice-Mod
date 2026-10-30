#include "criteria.h"
#include "log.h"

#include <cstdio>
#include <sstream>

namespace {

// Target time / orbs per mission, from dmc.wiki/dmc1:rank. Missions 15-17 use the corrected
// orb values the wiki notes (1400 / 1000 / 600); the in-game screenshots list 400 for all three.
criteria::Mission g_table[24] = {
    {},
    {6 * 60, 400},       // 1
    {7 * 60, 450},       // 2
    {3 * 60, 550},       // 3
    {4 * 60, 550},       // 4
    {2 * 60, 100},       // 5
    {3 * 60 + 30, 250},  // 6
    {3 * 60, 650},       // 7
    {3 * 60, 585},       // 8
    {8 * 60, 1500},      // 9
    {4 * 60 + 30, 600},  // 10
    {5 * 60 + 30, 1000}, // 11
    {4 * 60, 700},       // 12
    {2 * 60 + 30, 200},  // 13
    {5 * 60, 500},       // 14
    {10 * 60, 1400},     // 15
    {6 * 60, 1000},      // 16
    {7 * 60, 600},       // 17
    {6 * 60, 900},       // 18
    {3 * 60 + 30, 500},  // 19
    {4 * 60 + 30, 700},  // 20
    {5 * 60, 700},       // 21
    {8 * 60, 0},         // 22 (no orbs to find)
    {7 * 60, 600},       // 23
};

int ParseTime(const std::string& s) {
    int m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d:%d", &m, &sec) == 2) return m * 60 + sec;
    return atoi(s.c_str());
}

}  // namespace

void criteria::Load(const std::wstring& path) {
    FILE* fp = _wfopen(path.c_str(), L"r");
    if (!fp) return;
    std::string all;
    char chunk[512];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) all.append(chunk, got);
    fclose(fp);
    std::stringstream f(all);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        std::stringstream ss(line);
        std::string a, b, c;
        if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',') || !std::getline(ss, c, ',')) continue;
        const int mission = atoi(a.c_str());
        if (mission < 1 || mission > 23) continue;
        g_table[mission].timeSec = ParseTime(b);
        g_table[mission].orbs = atoi(c.c_str());
        ++n;
    }
    dlog::Write("[Criteria] loaded %d missions from dmc1_criteria.csv", n);
}

bool criteria::Get(int mission, Mission& out) {
    if (mission < 1 || mission > 23) return false;
    out = g_table[mission];
    return out.timeSec > 0;
}

criteria::Score criteria::Compute(const Mission& m, double frames, double tps, int orbs, int damage, int penalty,
                                  int bonus, bool dmd) {
    Score s;
    // Clear time: within target 500, +1..30s 300, +31s..2:00 200, +2:01..5:00 100, more 0.
    const int t = (int)(frames / tps);  // whole seconds
    const int over = t - m.timeSec;
    struct { int upTo; int pts; } timeB[] = {{0, 500}, {30, 300}, {120, 200}, {300, 100}};
    s.timePts = 0;
    for (auto& b : timeB) {
        if (over <= b.upTo) {
            s.timePts = b.pts;
            s.timeLeftSec = b.upTo - over;
            break;
        }
    }

    // Orbs found, as a share of the target: 100%+ 500, 80% 300, 50% 200, 30% 100, less 0.
    struct { int pct; int pts; } orbB[] = {{100, 500}, {80, 300}, {50, 200}, {30, 100}};
    if (m.orbs <= 0) {
        s.orbPts = 500;
    } else {
        s.orbPts = 0;
        int nextNeed = -1, nextPts = 0;
        for (auto& b : orbB) {
            const int need = (m.orbs * b.pct + 99) / 100;
            if (orbs >= need) { s.orbPts = b.pts; break; }
            nextNeed = need;
            nextPts = b.pts;
        }
        if (nextNeed >= 0) { s.orbsForNext = nextNeed - orbs; s.nextOrbPts = nextPts; }
    }

    // Damage taken: 0 500, 1-500 300, 501-1000 200, 1001-1500 100, more 0.
    struct { int upTo; int pts; } dmgB[] = {{0, 500}, {500, 300}, {1000, 200}, {1500, 100}};
    s.damagePts = 0;
    for (auto& b : dmgB) {
        if (damage <= b.upTo) {
            s.damagePts = b.pts;
            s.damageToDrop = b.upTo - damage + 1;
            break;
        }
    }

    s.penalty = penalty;
    s.bonus = bonus;
    s.total = s.timePts + s.orbPts + s.damagePts + bonus - penalty;

    // Rank: Standard S 1000+, A 800, B 500, C 0, D below. Dante Must Die S 700+, A 400, B 200, C 0.
    struct Th { int min; char r; };
    static const Th std_[] = {{1000, 'S'}, {800, 'A'}, {500, 'B'}, {0, 'C'}};
    static const Th dmd_[] = {{700, 'S'}, {400, 'A'}, {200, 'B'}, {0, 'C'}};
    const Th* tbl = dmd ? dmd_ : std_;
    s.rank = 'D';
    for (int i = 0; i < 4; ++i) {
        if (s.total >= tbl[i].min) {
            s.rank = tbl[i].r;
            if (i > 0) { s.pointsToNextRank = tbl[i - 1].min - s.total; s.nextRank = tbl[i - 1].r; }
            break;
        }
    }
    if (s.rank == 'D') { s.pointsToNextRank = -s.total; s.nextRank = 'C'; }
    return s;
}
