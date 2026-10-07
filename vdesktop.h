#pragma once

#include <windows.h>
#include <stdbool.h>

// Windows virtual desktops (Task View). Desktop numbers are 1-based everywhere in this API.
//
// Querying whether a window is on the current desktop uses the public IVirtualDesktopManager.
// Switching, creating and moving to desktops has no public API, so it goes through
// VirtualDesktopAccessor.dll (kept next to lightwm.exe), which tracks the undocumented
// interfaces across Windows builds.

bool vdInit(wchar_t* error, size_t errorCount);
void vdCleanup(void);

bool vdCanControl(void);
int vdCount(void);
int vdCurrent(void);
bool vdGoto(int number);
bool vdMoveWindow(HWND window, int number);
int vdWindowDesktop(HWND window);
bool vdWindowOnCurrent(HWND window);
