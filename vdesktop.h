#pragma once

#include <windows.h>
#include <stdbool.h>

// Windows virtual desktops (Task View). Desktop numbers are 1-based everywhere in this API.
//
// Windows has no public API to switch desktops, create them or move other programs' windows, so
// two backends are tried in order (backend "auto"):
//   1. VirtualDesktopAccessor.dll next to lightwm.exe - a maintained wrapper that tracks the
//      undocumented interfaces across Windows builds (the most reliable choice);
//   2. a built-in client for the same undocumented shell interfaces, covering Windows 10 (1809+)
//      and Windows 11. It is only used when the shell confirms the interface by its ID.
// Asking whether a window is on the current desktop uses the public IVirtualDesktopManager.

typedef enum {
	DESKTOP_BACKEND_AUTO = 0,
	DESKTOP_BACKEND_DLL,
	DESKTOP_BACKEND_BUILTIN
} DesktopBackend;

bool vdInit(int backendPreference, wchar_t* error, size_t errorCount);
void vdCleanup(void);

bool vdCanControl(void);
int vdCount(void);
int vdCurrent(void);
bool vdGoto(int number);
bool vdMoveWindow(HWND window, int number);
int vdWindowDesktop(HWND window);
bool vdWindowOnCurrent(HWND window);
