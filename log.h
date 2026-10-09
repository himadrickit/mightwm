#pragma once

#include <stdbool.h>
#include <wchar.h>

// Optional diagnostics: with `debug-log true` in config.kdl, events are appended to lightwm.log
// next to lightwm.exe. Off by default.
void logInit(bool enabled);
void logWrite(const char* format, ...);
void logClose(void);

bool logEnabled(void);
const wchar_t* logPath(void);
