#pragma once
#include <Arduino.h>

struct __attribute__((packed)) SensorRecord {
  uint32_t timestamp;     // unix time, seconds (0 if not yet NTP-synced)
  int16_t  tempCx100;     // temperature, hundredths of a degree C
  int16_t  humidityX100;  // relative humidity, hundredths of a percent
};

struct FileInfo {
  char name[32];
  uint32_t size;
};

bool storageBegin();
bool storageAppend(const SensorRecord &rec);
void storagePruneOldDays(uint32_t nowUnix, uint8_t retentionDays);

// Lists files at the LittleFS root into out[], up to maxCount. Returns the count.
int storageListFiles(FileInfo *out, int maxCount);

// Opens `path` for chunked reading. Any previously open read is closed first.
// On success, *outSize receives the file size and the file is ready for
// storageReadNextChunk(). Returns false if the file doesn't exist.
bool storageOpenForRead(const String &path, uint32_t *outSize);

// Reads up to maxLen bytes from the file opened by storageOpenForRead(),
// advancing the read position. Closes the file automatically once the last
// byte has been read. Returns the number of bytes actually read.
size_t storageReadNextChunk(uint8_t *buf, size_t maxLen);

// Closes the file opened by storageOpenForRead(), if any. Safe to call when
// nothing is open.
void storageCloseRead();

// Deletes `path`. Returns false if it doesn't exist or couldn't be removed.
bool storageDeleteFile(const String &path);
