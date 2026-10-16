// settings.cpp -- see settings.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"
#include "../hal/hal.h"

Settings g_settings;

std::string sdPath(const char *rel) {
  std::string p = hal_sd_root();
  if (rel && *rel) {
    if (rel[0] != '/') p += '/';
    p += rel;
  }
  return p;
}

bool isDir(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkOne(const std::string &p) {
#if defined(_WIN32)
  return mkdir(p.c_str()) == 0 || isDir(p);
#else
  return mkdir(p.c_str(), 0777) == 0 || isDir(p);
#endif
}

bool makeDirs(const std::string &path) {
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/' && path[i - 1] != ':') mkOne(path.substr(0, i));
  return mkOne(path);
}

static std::string cfgPath() { return sdPath("emu16/emu16.cfg"); }

void settingsLoad() {
  FILE *f = fopen(cfgPath().c_str(), "r");
  if (!f) return;
  char line[300];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *v = eq + 1;
    v[strcspn(v, "\r\n")] = 0;
    if (!strcmp(line, "volume"))         g_settings.volume = atoi(v);
    else if (!strcmp(line, "frameskip")) g_settings.frameskip = atoi(v);
    else if (!strcmp(line, "showfps"))   g_settings.showFps = atoi(v) != 0;
    else if (!strcmp(line, "lastdir"))   g_settings.lastDir = v;
  }
  fclose(f);
  if (g_settings.volume < 0 || g_settings.volume > 10) g_settings.volume = 7;
  if (g_settings.frameskip < 0 || g_settings.frameskip > 3) g_settings.frameskip = 0;
}

void settingsSave() {
  makeDirs(sdPath("emu16"));
  FILE *f = fopen(cfgPath().c_str(), "w");
  if (!f) { hal_log("settings: cannot write %s", cfgPath().c_str()); return; }
  fprintf(f, "volume=%d\nframeskip=%d\nshowfps=%d\nlastdir=%s\n", g_settings.volume,
          g_settings.frameskip, g_settings.showFps ? 1 : 0, g_settings.lastDir.c_str());
  fclose(f);
}
