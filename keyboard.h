#pragma once

#include <windows.h>
#include <stdbool.h>
#include "config.h"

// Posted to the main thread when a keybind fires; wParam is the binding id (index + 1).
#define WM_LWM_ACTION (WM_APP + 1)

// Keybinds fire through two paths that back each other up:
//  1. the low-level keyboard hook (altdrag.c), which sees keys first and so beats other programs
//     that grabbed the same combo;
//  2. RegisterHotKey, which still works where hooks are blind (an elevated window has focus).
void keyboardInit(const Config* cfg);
int keyboardMatch(UINT mods, UINT vk);
const Binding* keyboardLookup(WPARAM bindingId);

// Registers every binding as a system hotkey. A combo another program owns is reported in
// `message` but stays active through the hook. Returns false when something was reported.
bool keyboardRegister(wchar_t* message, size_t messageCount);
void keyboardUnregister(void);

// True when this action id fired a moment ago through the other path (hook vs. hotkey).
bool keyboardIsDuplicate(WPARAM bindingId);
