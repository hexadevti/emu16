// md_state.cpp -- the storage behind Gwenesis' save-state calls. Each module opens a named
// section and gets/sets tagged ints and buffers; here that is a flat in-memory list of
// records {section, tag, length, bytes}, written to / read from a file in one go.
//
// File: "EM16MDS1", u32 record count, then per record: u8 name length, "section:tag",
// u32 length, data.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "../hal/hal.h"

extern "C" {
#include "gwenesis/gwenesis_savestate.h"
}

static const char kMagic[8] = { 'E', 'M', '1', '6', 'M', 'D', 'S', '1' };

struct Record {
  std::string key;
  std::vector<uint8_t> data;
};

static std::vector<Record> s_records;

struct SaveState {
  std::string section;
};

static SaveState s_state;

static std::string keyOf(SaveState *st, const char *tag) { return st->section + ":" + tag; }

static Record *find(const std::string &key) {
  for (auto &r : s_records) if (r.key == key) return &r;
  return nullptr;
}

static void put(const std::string &key, const void *p, int len) {
  Record *r = find(key);
  if (!r) { s_records.push_back({ key, {} }); r = &s_records.back(); }
  r->data.assign((const uint8_t *)p, (const uint8_t *)p + len);
}

extern "C" {

SaveState *saveGwenesisStateOpenForRead(const char *name)  { s_state.section = name; return &s_state; }
SaveState *saveGwenesisStateOpenForWrite(const char *name) { s_state.section = name; return &s_state; }

int saveGwenesisStateGet(SaveState *st, const char *tag) {
  Record *r = find(keyOf(st, tag));
  int32_t v = 0;
  if (r && r->data.size() == sizeof(v)) memcpy(&v, r->data.data(), sizeof(v));
  else hal_log("state: missing %s", keyOf(st, tag).c_str());
  return v;
}

void saveGwenesisStateSet(SaveState *st, const char *tag, int value) {
  const int32_t v = value;
  put(keyOf(st, tag), &v, sizeof(v));
}

void saveGwenesisStateGetBuffer(SaveState *st, const char *tag, void *buf, int len) {
  Record *r = find(keyOf(st, tag));
  if (!r) { hal_log("state: missing %s", keyOf(st, tag).c_str()); return; }
  memcpy(buf, r->data.data(), r->data.size() < (size_t)len ? r->data.size() : (size_t)len);
}

void saveGwenesisStateSetBuffer(SaveState *st, const char *tag, void *buf, int len) {
  put(keyOf(st, tag), buf, len);
}

} // extern "C"

bool mdStateWrite(FILE *f) {
  s_records.clear();
  gwenesis_save_state();
  const uint32_t n = (uint32_t)s_records.size();
  bool ok = fwrite(kMagic, 1, 8, f) == 8 && fwrite(&n, 4, 1, f) == 1;
  for (auto &r : s_records) {
    if (!ok) break;
    const uint8_t kl = (uint8_t)r.key.size();
    const uint32_t dl = (uint32_t)r.data.size();
    ok = fwrite(&kl, 1, 1, f) == 1 && fwrite(r.key.data(), 1, kl, f) == kl &&
         fwrite(&dl, 4, 1, f) == 1 && fwrite(r.data.data(), 1, dl, f) == dl;
  }
  s_records.clear();
  s_records.shrink_to_fit();
  return ok;
}

bool mdStateRead(FILE *f) {
  char magic[8];
  uint32_t n = 0;
  if (fread(magic, 1, 8, f) != 8 || memcmp(magic, kMagic, 8) || fread(&n, 4, 1, f) != 1) return false;
  s_records.clear();
  bool ok = true;
  for (uint32_t i = 0; i < n && ok; i++) {
    uint8_t kl;
    uint32_t dl;
    char key[256];
    ok = fread(&kl, 1, 1, f) == 1 && fread(key, 1, kl, f) == kl && fread(&dl, 4, 1, f) == 1 && dl < (1u << 20);
    if (!ok) break;
    Record r;
    r.key.assign(key, kl);
    r.data.resize(dl);
    ok = fread(r.data.data(), 1, dl, f) == dl;
    s_records.push_back(std::move(r));
  }
  if (ok) gwenesis_load_state();
  s_records.clear();
  s_records.shrink_to_fit();
  return ok;
}
