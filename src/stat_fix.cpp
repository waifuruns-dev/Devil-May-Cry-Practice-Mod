#include "stat_fix.h"
#include "config.h"
#include "log.h"
#include "watch.h"

#include <windows.h>

namespace {

bool g_ready = false;
AddressExpr g_hpBar, g_dtBar, g_maxHp, g_maxDt, g_maxGauge, g_hp, g_dt;
int g_lastSerial = 0;
ULONGLONG g_activeUntil = 0;

int Read(const AddressExpr& a, ValueType t) {
    double v;
    return ReadWatch(a, t, v) ? (int)v : -1;
}

}  // namespace

void stat_fix::Init() {
    const Config& c = g_config;
    g_ready = c.statFix &&
              ParseAddress(c.hpBarAddr, g_hpBar) && ParseAddress(c.dtBarAddr, g_dtBar) &&
              ParseAddress(c.maxHpAddr, g_maxHp) && ParseAddress(c.maxDtAddr, g_maxDt) &&
              ParseAddress(c.maxGaugeAddr, g_maxGauge) &&
              ParseAddress(c.hpAddr, g_hp) && ParseAddress(c.dtAddr, g_dt);
    if (c.statFix && !g_ready) dlog::Write("[StatFix] couldn't parse one of the [StatFix] addresses; disabled");
}

void stat_fix::Update(int serial) {
    if (!g_ready) return;

    const ULONGLONG now = GetTickCount64();
    if (serial != g_lastSerial) {
        g_lastSerial = serial;
        g_activeUntil = now + 3000;  // keep checking for the first 3 seconds of the mission
    }
    if (now > g_activeUntil) return;

    const int hpBars = Read(g_hpBar, ValueType::U8);
    const int dtBars = Read(g_dtBar, ValueType::U8);
    const int maxHp = Read(g_maxHp, ValueType::U16);
    const int maxDt = Read(g_maxDt, ValueType::U16);
    if (hpBars < 1 || hpBars > 30 || dtBars < 0 || dtBars > 10 || maxHp < 0 || maxDt < 0) return;

    // Two DT scales: the gauge counts 120 per rune, DT time (while triggered) 200 per rune.
    const int wantHp = hpBars * 100;
    const int wantDt = dtBars * 200;
    const int wantGauge = dtBars * 120;
    const int maxGauge = Read(g_maxGauge, ValueType::U16);
    if (maxHp != wantHp && WriteU16(g_maxHp, (uint16_t)wantHp))
        dlog::Write("[StatFix] Max HP %d -> %d", maxHp, wantHp);
    if (maxDt != wantDt && WriteU16(g_maxDt, (uint16_t)wantDt))
        dlog::Write("[StatFix] Max DT time %d -> %d", maxDt, wantDt);
    if (maxGauge >= 0 && maxGauge != wantGauge && WriteU16(g_maxGauge, (uint16_t)wantGauge))
        dlog::Write("[StatFix] Max DT gauge %d -> %d", maxGauge, wantGauge);

    // Never leave current values above the (new) maximum.
    const int hp = Read(g_hp, ValueType::U16);
    if (hp > wantHp) WriteU16(g_hp, (uint16_t)wantHp);
    const int dt = Read(g_dt, ValueType::U16);
    if (dt > wantGauge) WriteU16(g_dt, (uint16_t)wantGauge);
}
