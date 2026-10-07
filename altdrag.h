#pragma once

#include <windows.h>
#include <stdbool.h>
#include "config.h"

// Built-in "hold a modifier and drag anywhere on a window" move/resize, in the spirit of AltDrag.
bool altdragInstall(const Config* cfg);
void altdragUninstall(void);

// Windows silently drops slow low-level hooks and other programs can insert theirs in front of ours.
// Re-installing while idle puts us back first in line.
void altdragRefreshHooks(void);
