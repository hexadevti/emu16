// browser.h -- the ROM picker.
#pragma once

#include <string>
#include "../../core.h"

// Lists folders and the core's ROM files, starting where the browser last was (else
// /roms/<shortName>). true + the full path when a ROM was chosen, false on Esc at the top.
bool browseRom(const EmuCore &core, std::string &outPath);
