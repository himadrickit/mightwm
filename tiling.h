#pragma once

#include <windows.h>
#include <stdbool.h>
#include "config.h"

typedef enum { DIR_LEFT = 0, DIR_RIGHT, DIR_UP, DIR_DOWN } Direction;

void tilingInit(const Config* cfg);
void tilingReload(const Config* cfg);
void tilingRetile(void);

bool tilingWantsEvent(DWORD event, HWND window);
bool tilingIsCandidate(HWND window);
bool tilingIsTiled(HWND window);
bool tilingModeActive(void);
void tilingSetSuspended(bool suspended);

void tilingFocusRelative(int step);
void tilingFocusDirection(Direction direction);
void tilingMoveDirection(Direction direction);
void tilingToggleFloating(void);
void tilingToggleMonocle(void);
void tilingToggleFullscreen(void);
void tilingToggleEnabled(void);
void tilingCloseWindow(void);

// Windows' own title-bar drag/resize (EVENT_SYSTEM_MOVESIZESTART / END).
void tilingNativeMoveStart(HWND window);
void tilingNativeMoveEnd(HWND window);

// Called by altdrag when a drag ends.
void tilingDragDrop(HWND window, POINT cursor);
void tilingResizeDrop(HWND window, RECT frame, int edgeX);
