#include <LittleFS.h>

#include "file_manager.h"

namespace {

bool g_file_system_ready = false;

}

bool setupFileManager() {
  g_file_system_ready = LittleFS.begin(false);
  return g_file_system_ready;
}

bool fileSystemReady() {
  return g_file_system_ready;
}

bool pathExists(const char *path) {
  if (!g_file_system_ready) {
    return false;
  }

  return LittleFS.exists(path);
}

bool ensureDirectory(const char *path) {
  if (!g_file_system_ready) {
    return false;
  }

  if (LittleFS.exists(path)) {
    return true;
  }

  return LittleFS.mkdir(path);
}

bool readFileText(const char *path, String &contents) {
  contents = "";

  if (!g_file_system_ready) {
    return false;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }

  contents = file.readString();
  file.close();
  return true;
}

bool writeFileText(const char *path, const String &contents) {
  if (!g_file_system_ready) {
    return false;
  }

  File file = LittleFS.open(path, "w");
  if (!file) {
    return false;
  }

  size_t written = file.print(contents);
  file.close();
  return written == contents.length();
}

bool deleteFilePath(const char *path) {
  if (!g_file_system_ready || !LittleFS.exists(path)) {
    return false;
  }

  return LittleFS.remove(path);
}

size_t listFilesInDirectory(const char *directory_path, String *entries, size_t max_entries) {
  if (!g_file_system_ready || entries == nullptr || max_entries == 0) {
    return 0;
  }

  File root = LittleFS.open(directory_path, "r");
  if (!root || !root.isDirectory()) {
    return 0;
  }

  size_t count = 0;
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && count < max_entries) {
      entries[count++] = String(entry.name());
    }

    File next_entry = root.openNextFile();
    entry.close();
    entry = next_entry;
  }

  root.close();
  return count;
}
