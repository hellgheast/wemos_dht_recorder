#include "storage.h"
#include <LittleFS.h>
#include <time.h>

// Anything before this is treated as "clock not yet NTP-synced" rather than
// a real calendar date (year ~2001 in unix time).
static const uint32_t UNSYNCED_THRESHOLD = 1000000000UL;

static String dayFileName(uint32_t unixTime) {
  if (unixTime < UNSYNCED_THRESHOLD) {
    return "/log_unsynced.bin";
  }
  time_t t = (time_t)unixTime;
  struct tm tmStruct;
  gmtime_r(&t, &tmStruct);
  char buf[24];
  snprintf(buf, sizeof(buf), "/log_%04d-%02d-%02d.bin",
           tmStruct.tm_year + 1900, tmStruct.tm_mon + 1, tmStruct.tm_mday);
  return String(buf);
}

static File g_readFile;

bool storageBegin() {
  return LittleFS.begin();
}

int storageListFiles(FileInfo *out, int maxCount) {
  int count = 0;
  Dir dir = LittleFS.openDir("/");
  while (count < maxCount && dir.next()) {
    String name = dir.fileName();
    if (!name.startsWith("/")) {
      name = "/" + name;
    }
    strncpy(out[count].name, name.c_str(), sizeof(out[count].name) - 1);
    out[count].name[sizeof(out[count].name) - 1] = '\0';
    out[count].size = dir.fileSize();
    count++;
  }
  return count;
}

bool storageOpenForRead(const String &path, uint32_t *outSize) {
  storageCloseRead();
  g_readFile = LittleFS.open(path, "r");
  if (!g_readFile) {
    return false;
  }
  *outSize = g_readFile.size();
  return true;
}

size_t storageReadNextChunk(uint8_t *buf, size_t maxLen) {
  if (!g_readFile) {
    return 0;
  }
  size_t n = g_readFile.read(buf, maxLen);
  if (g_readFile.position() >= g_readFile.size()) {
    g_readFile.close();
  }
  return n;
}

void storageCloseRead() {
  if (g_readFile) {
    g_readFile.close();
  }
}

bool storageDeleteFile(const String &path) {
  if (!LittleFS.exists(path)) {
    return false;
  }
  return LittleFS.remove(path);
}

bool storageAppend(const SensorRecord &rec) {
  String path = dayFileName(rec.timestamp);
  File f = LittleFS.open(path, "a");
  if (!f) {
    return false;
  }
  size_t written = f.write((const uint8_t *)&rec, sizeof(rec));
  f.close();
  return written == sizeof(rec);
}

void storagePruneOldDays(uint32_t nowUnix, uint8_t retentionDays) {
  if (nowUnix < UNSYNCED_THRESHOLD) {
    return;  // no real calendar yet, nothing meaningful to prune by date
  }
  time_t cutoff = (time_t)nowUnix - (time_t)retentionDays * 86400L;

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    String name = dir.fileName();
    if (!name.startsWith("/")) {
      name = "/" + name;
    }

    int y, m, d;
    if (sscanf(name.c_str(), "/log_%d-%d-%d.bin", &y, &m, &d) == 3) {
      struct tm fileTm = {};
      fileTm.tm_year = y - 1900;
      fileTm.tm_mon = m - 1;
      fileTm.tm_mday = d;
      time_t fileTime = mktime(&fileTm);
      if (fileTime < cutoff) {
        LittleFS.remove(name);
      }
    }
  }
}
