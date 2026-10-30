#pragma once
#include <string>

// Copies DMC1's save files (dmc1.sav, options.sav; every Steam account on this PC) to <mod folder>\dmc1_save_backups\
// <account>\<date and time>\ when the game starts, before it reads them. Keeps the newest 20.
namespace backup {

void BackUpSaves(const std::wstring& modFolder);

}  // namespace backup
