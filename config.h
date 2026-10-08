#pragma once

#include <windows.h>
#include <stdbool.h>
#include "layout.h"

typedef enum {
	ACT_NONE = 0,
	ACT_FOCUS_NEXT, ACT_FOCUS_PREV,
	ACT_FOCUS_LEFT, ACT_FOCUS_RIGHT, ACT_FOCUS_UP, ACT_FOCUS_DOWN,
	ACT_MOVE_LEFT, ACT_MOVE_RIGHT, ACT_MOVE_UP, ACT_MOVE_DOWN,
	ACT_CLOSE, ACT_TOGGLE_FLOAT, ACT_MONOCLE, ACT_FULLSCREEN,
	ACT_RETILE, ACT_TOGGLE_TILING, ACT_RELOAD, ACT_QUIT,
	ACT_SPAWN, ACT_GOTO, ACT_SEND, ACT_WORKSPACE_NEXT, ACT_WORKSPACE_PREV
} Action;

enum { BTN_NONE = 0, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };

#define MAX_RULES 64

typedef struct {
	UINT mods;
	UINT vk;
	Action action;
	int arg;
	wchar_t* text;
} Binding;

typedef struct {
	int gap;
	int masterPercent;
	LayoutKind layout;
	bool warpCursor;
	int floatPercent;
	bool followFocus;
	bool blockWindowsDesktopKeys;
	int desktopBackend;

	bool altdragEnabled;
	UINT altdragMods;
	int altdragMoveButton;
	int altdragResizeButton;

	wchar_t* floatRules[MAX_RULES];
	int floatRuleCount;

	Binding* binds;
	int bindCount;
	int bindCapacity;
} Config;

// Always fills cfg (built-in defaults when the file is broken). Returns false when there were
// problems; `message` then describes them.
bool configLoad(Config* cfg, wchar_t* message, size_t messageCount);
void configFree(Config* cfg);
const wchar_t* configPath(void);
