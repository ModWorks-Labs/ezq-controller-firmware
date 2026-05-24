#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <Arduino.h>

bool setupFileManager();
bool fileSystemReady();
bool pathExists(const char *path);
bool ensureDirectory(const char *path);
bool readFileText(const char *path, String &contents);
bool writeFileText(const char *path, const String &contents);
bool deleteFilePath(const char *path);
size_t listFilesInDirectory(const char *directory_path, String *entries, size_t max_entries);

#endif
