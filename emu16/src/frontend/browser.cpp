// browser.cpp -- picks a ROM: folders first, then files with one of the core's extensions.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>
#include "browser.h"
#include "settings.h"
#include "ui.h"
#include "../hal/hal.h"

static bool extMatches(const char *name, const char *exts) {
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) return false;
  const size_t n = strlen(dot);
  for (const char *p = exts; (p = strchr(p, '.')) != nullptr; p++) {
    const char *end = strchr(p + 1, '.');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !strncasecmp(p, dot, n)) return true;
  }
  return false;
}

struct Entry {
  std::string name;
  bool dir;
};

static std::string parentOf(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos || s == 0 ? p : p.substr(0, s);
}

bool browseRom(const EmuCore &core, std::string &outPath) {
  const std::string root = hal_sd_root();
  std::string dir = g_settings.lastDir;
  if (dir.empty() || dir.compare(0, root.size(), root) || !isDir(dir)) {
    dir = sdPath((std::string("roms/") + core.shortName).c_str());
    if (!isDir(dir)) dir = root;
  }
  std::string selectName;

  for (;;) {
    std::vector<Entry> entries;
    if (DIR *d = opendir(dir.c_str())) {
      while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const bool isD = isDir(dir + "/" + e->d_name);
        if (isD || extMatches(e->d_name, core.romExts)) entries.push_back({ e->d_name, isD });
      }
      closedir(d);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
      if (a.dir != b.dir) return a.dir;
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    const bool atRoot = dir == root;
    if (!atRoot) entries.insert(entries.begin(), { "..", true });

    std::vector<std::string> labels(entries.size());
    std::vector<ui::Item> items(entries.size());
    int sel = 0;
    for (size_t i = 0; i < entries.size(); i++) {
      labels[i] = entries[i].dir && entries[i].name != ".." ? entries[i].name + "/" : entries[i].name;
      items[i] = { labels[i].c_str(), nullptr };
      if (entries[i].name == selectName) sel = (int)i;
    }
    selectName.clear();

    std::string title = core.name;
    const std::string shown = dir.size() > root.size() ? dir.substr(root.size()) : std::string("/");
    char footer[96];
    snprintf(footer, sizeof(footer), "%s   Enter: open   Esc: back", shown.c_str());
    if (entries.empty()) items.push_back({ "(no ROMs here)", nullptr });

    const uint8_t k = ui::list(title.c_str(), items.data(), (int)items.size(), &sel, footer);
    if (k == KEY_ESC || ((k == KEY_BACKSPACE || k == KEY_LEFT) && atRoot)) return false;
    if (k == KEY_BACKSPACE || k == KEY_LEFT) {
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
      continue;
    }
    if (k != KEY_ENTER || entries.empty()) continue;
    const Entry &e = entries[sel];
    if (e.name == "..") {
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
    } else if (e.dir) {
      dir += "/" + e.name;
    } else {
      outPath = dir + "/" + e.name;
      g_settings.lastDir = dir;
      return true;
    }
  }
}
