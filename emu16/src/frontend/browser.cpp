// browser.cpp -- picks a ROM: folders first, then files with one of the core's extensions.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
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

    // files show without their extension, which goes in the value column as a tag
    std::vector<std::string> labels(entries.size()), tags(entries.size());
    std::vector<ui::Item> items(entries.size());
    int sel = !atRoot && entries.size() > 1 ? 1 : 0;      // the first entry, not "Parent folder"
    for (size_t i = 0; i < entries.size(); i++) {
      const Entry &e = entries[i];
      if (e.name == "..") {
        labels[i] = "Parent folder";
        items[i] = { labels[i].c_str(), nullptr, ui::ICON_UP };
      } else if (e.dir) {
        labels[i] = e.name;
        items[i] = { labels[i].c_str(), nullptr, ui::ICON_FOLDER };
      } else {
        const size_t dot = e.name.find_last_of('.');
        labels[i] = e.name.substr(0, dot);
        for (size_t c = dot + 1; c < e.name.size(); c++) tags[i] += (char)toupper((unsigned char)e.name[c]);
        items[i] = { labels[i].c_str(), tags[i].c_str(), ui::ICON_CART };
      }
      if (e.name == selectName) sel = (int)i;
    }
    selectName.clear();

    const std::string shown = dir.size() > root.size() ? dir.substr(root.size()) : std::string("/");
    if (entries.empty()) items.push_back({ "No ROMs in this folder", nullptr, ui::ICON_NONE });

    ui::Screen scr;
    scr.title = core.name;
    scr.subtitle = shown.c_str();
    scr.hints = atRoot ? "Enter|Open;Esc|Back" : "Enter|Open;Bs|Up;Esc|Back";
    const uint8_t k = ui::list(scr, items.data(), (int)items.size(), &sel);
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
