#pragma once

#include <windows.h>
#include <stdbool.h>

enum {
	TRAY_TOGGLE_TILING = 1,
	TRAY_RETILE,
	TRAY_RELOAD,
	TRAY_EDIT_CONFIG,
	TRAY_QUIT
};

typedef void (*TrayHandler)(int command);

// System tray icon: right-click for the menu, double-click toggles tiling.
bool trayInit(HINSTANCE instance, TrayHandler handler);
void trayUpdate(bool tilingEnabled, int desktop);
void trayCleanup(void);
