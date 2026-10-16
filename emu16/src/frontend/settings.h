// settings.h -- user settings, kept as "key=value" lines in <sd>/emu16/emu16.cfg.
#pragma once

#include <stdint.h>
#include <string>

struct Settings {
  int volume = 7;            // 0..10
  int frameskip = 0;         // 0 = auto, N = draw 1 frame in N+1
  bool showFps = false;
  std::string lastDir;       // where the ROM browser was last
};

extern Settings g_settings;

void settingsLoad();
void settingsSave();

// <sd root>/<rel>, and a mkdir -p for it (the path's own last component included).
std::string sdPath(const char *rel);
bool makeDirs(const std::string &path);
bool isDir(const std::string &path);
