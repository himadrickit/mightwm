#pragma once

#include <windows.h>
#include <stdbool.h>
#include "config.h"

typedef enum { DIR_LEFT = 0, DIR_RIGHT, DIR_UP, DIR_DOWN } Direction;

typedef void (*TilingScheduler)(UINT delayMs);

void tilingInit(const Config* cfg);
void tilingSetScheduler(TilingScheduler scheduler);
bool tilingIsEnabled(void);
// Invisible resize-border insets of a window, measured at rest and cached (see tiling.c).
void tilingGetInsets(HWND window, int* left, int* top, int* right, int* bottom);
void tilingReload(const Config* cfg);
void tilingRetile(void);

bool tilingWantsEvent(DWORD event, HWND window);
bool tilingIsCandidate(HWND window);
bool tilingIsTiled(HWND window);
bool tilingModeActive(void);
void tilingSetSuspended(bool suspended);

void tilingFocusRelative(int step);
void tilingFocusMaster(void);

// ---- per-workspace focus memory ----
// Each workspace remembers the window that had focus; switching back restores it (falling back
// to the master window). Every step is written to the debug log.
void tilingRememberFocus(HWND window, int desktop);
HWND tilingWorkspaceFocusTarget(int desktop);
bool tilingIsForeground(HWND window);
void tilingFocusWindow(HWND window);
void tilingClearAlert(HWND window);
void tilingDescribe(HWND window, char* out, size_t size);
void tilingLogFocusTable(void);
// Focuses the master window per the auto-focus setting (after closes / desktop switches).
void tilingAutoFocus(void);
void tilingFocusDirection(Direction direction);
void tilingMoveDirection(Direction direction);
void tilingToggleFloating(void);
void tilingToggleMonocle(void);
void tilingToggleFullscreen(void);
void tilingToggleEnabled(void);
void tilingAdjustMaster(int deltaPercent);
void tilingCycleMaster(void);
void tilingCloseWindow(void);

// Windows' own title-bar drag/resize (EVENT_SYSTEM_MOVESIZESTART / END).
void tilingNativeMoveStart(HWND window);
void tilingNativeMoveEnd(HWND window);

// Called by altdrag when a drag ends.
void tilingDragDrop(HWND window, POINT cursor, RECT frame);
void tilingResizeDrop(HWND window, RECT frame, int edgeX);
