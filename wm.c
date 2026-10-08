#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include "altdrag.h"
#include "config.h"
#include "error.h"
#include "keyboard.h"
#include "tiling.h"
#include "tray.h"
#include "vdesktop.h"

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

#define EXIT_OK 0
#define EXIT_FAILED 1

// After we switch desktops ourselves, ignore focus events for a moment so follow-focus
// does not fight the switch.
#define SWITCH_SETTLE_MS 400
#define RETILE_DELAY_MS 40
#define HOOK_REFRESH_MS 15000

static Config config;
static UINT_PTR retileTimer = 0;
static UINT_PTR hookRefreshTimer = 0;
static DWORD lastSwitchTick = 0;
static int lastDesktop = 0;
static bool running = true;
#define EVENT_HOOK_COUNT 5
static HWINEVENTHOOK eventHooks[EVENT_HOOK_COUNT];

static void refreshTray(void);

static void scheduleRetile(UINT delay)
{
	if (!retileTimer) {
		retileTimer = SetTimer(NULL, 0, delay, NULL);
	}
}

static void showProblems(const wchar_t* text)
{
	if (text[0]) {
		reportGeneralError(text);
	}
}

static void setDpiAwareness(void)
{
	typedef BOOL (WINAPI *SetContextFn)(HANDLE);
	SetContextFn setContext = (SetContextFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");

	if (!setContext || !setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
		SetProcessDPIAware();
	}
}

static void switchDesktop(int number)
{
	if (!vdCanControl() || number < 1) return;

	lastSwitchTick = GetTickCount();
	vdGoto(number);
	scheduleRetile(RETILE_DELAY_MS * 2);
	refreshTray();
}

static void spawnCommand(const wchar_t* command)
{
	wchar_t expanded[2048];
	if (!ExpandEnvironmentStringsW(command, expanded, 2048)) return;

	wchar_t* file = expanded;
	wchar_t* params = NULL;

	if (*file == L'"') {
		file++;
		wchar_t* close = wcschr(file, L'"');
		if (close) {
			*close = 0;
			params = close + 1;
		}
	} else {
		wchar_t* space = wcschr(file, L' ');
		if (space) {
			*space = 0;
			params = space + 1;
		}
	}

	if (params) {
		while (*params == L' ') params++;
		if (*params == 0) params = NULL;
	}

	ShellExecuteW(NULL, L"open", file, params, NULL, SW_SHOWNORMAL);
}

static void reloadConfig(void)
{
	wchar_t problems[2048];

	keyboardUnregister();
	configFree(&config);
	configLoad(&config, problems, 2048);
	tilingReload(&config);

	wchar_t keyProblems[2048];
	keyboardRegister(keyProblems, 2048);
	wcsncat(problems, keyProblems, 2048 - wcslen(problems) - 1);
	showProblems(problems);

	tilingRetile();
}

static void refreshTray(void)
{
	trayUpdate(tilingIsEnabled(), vdCurrent());
}

static void reloadConfig(void);

static void trayHandler(int command)
{
	switch (command) {
		case TRAY_TOGGLE_TILING: tilingToggleEnabled(); refreshTray(); break;
		case TRAY_RETILE: tilingRetile(); break;
		case TRAY_RELOAD: reloadConfig(); break;
		case TRAY_EDIT_CONFIG: ShellExecuteW(NULL, L"open", configPath(), NULL, NULL, SW_SHOWNORMAL); break;
		case TRAY_QUIT: running = false; PostQuitMessage(0); break;
		default: break;
	}
}

static void runAction(const Binding* b)
{
	switch (b->action) {
		case ACT_FOCUS_NEXT: tilingFocusRelative(1); break;
		case ACT_FOCUS_PREV: tilingFocusRelative(-1); break;
		case ACT_FOCUS_LEFT: tilingFocusDirection(DIR_LEFT); break;
		case ACT_FOCUS_RIGHT: tilingFocusDirection(DIR_RIGHT); break;
		case ACT_FOCUS_UP: tilingFocusDirection(DIR_UP); break;
		case ACT_FOCUS_DOWN: tilingFocusDirection(DIR_DOWN); break;
		case ACT_MOVE_LEFT: tilingMoveDirection(DIR_LEFT); break;
		case ACT_MOVE_RIGHT: tilingMoveDirection(DIR_RIGHT); break;
		case ACT_MOVE_UP: tilingMoveDirection(DIR_UP); break;
		case ACT_MOVE_DOWN: tilingMoveDirection(DIR_DOWN); break;
		case ACT_CLOSE: tilingCloseWindow(); break;
		case ACT_TOGGLE_FLOAT: tilingToggleFloating(); break;
		case ACT_MONOCLE: tilingToggleMonocle(); break;
		case ACT_FULLSCREEN: tilingToggleFullscreen(); break;
		case ACT_RETILE: tilingRetile(); break;
		case ACT_TOGGLE_TILING: tilingToggleEnabled(); refreshTray(); break;
		case ACT_RELOAD: reloadConfig(); break;
		case ACT_QUIT: running = false; break;
		case ACT_SPAWN: if (b->text) spawnCommand(b->text); break;
		case ACT_GOTO: switchDesktop(b->arg); break;
		case ACT_SEND: {
			HWND window = GetAncestor(GetForegroundWindow(), GA_ROOT);
			if (window && vdCanControl() && vdMoveWindow(window, b->arg)) {
				scheduleRetile(RETILE_DELAY_MS);
			}
			break;
		}
		case ACT_WORKSPACE_NEXT:
		case ACT_WORKSPACE_PREV: {
			int count = vdCount(), current = vdCurrent();
			if (count > 0 && current > 0) {
				int step = b->action == ACT_WORKSPACE_NEXT ? 1 : -1;
				switchDesktop((current - 1 + step + count) % count + 1);
			}
			break;
		}
		default: break;
	}
}

// A window got focus: follow it to its desktop if needed, and retile after desktop switches.
static void onForeground(HWND window)
{
	int current = vdCurrent();
	bool settled = GetTickCount() - lastSwitchTick > SWITCH_SETTLE_MS;

	if (config.followFocus && vdCanControl() && settled && tilingIsCandidate(window) && !vdWindowOnCurrent(window)) {
		int desktop = vdWindowDesktop(window);
		if (desktop > 0 && desktop != current) {
			switchDesktop(desktop);
			return;
		}
	}

	if (current != lastDesktop) {
		lastDesktop = current;
		refreshTray();
		scheduleRetile(RETILE_DELAY_MS);
	} else if (tilingModeActive()) {
		scheduleRetile(RETILE_DELAY_MS / 2);
	}
}

static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event, HWND window, LONG idObject, LONG idChild, DWORD thread, DWORD time)
{
	if (!window || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

	if (event == EVENT_SYSTEM_FOREGROUND) {
		onForeground(window);
	} else if (event == EVENT_SYSTEM_MOVESIZESTART) {
		tilingNativeMoveStart(window);
	} else if (event == EVENT_SYSTEM_MOVESIZEEND) {
		tilingNativeMoveEnd(window);
	} else if (tilingWantsEvent(event, window)) {
		scheduleRetile(RETILE_DELAY_MS);
	}
}

static void installEventHooks(void)
{
	const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

	eventHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, winEventProc, 0, 0, flags);
	eventHooks[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, winEventProc, 0, 0, flags);
	eventHooks[2] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, NULL, winEventProc, 0, 0, flags);
	eventHooks[4] = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, winEventProc, 0, 0, flags);
	eventHooks[3] = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, NULL, winEventProc, 0, 0, flags);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show)
{
	HANDLE mutex = CreateMutexW(NULL, TRUE, L"Global\\LightWMIsCurrentlyRunning");
	if (mutex == NULL) {
		reportWin32Error(L"Failed creating the single instance mutex");
		return EXIT_FAILED;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		reportGeneralError(L"LightWM is already running, exiting");
		CloseHandle(mutex);
		return EXIT_FAILED;
	}

	setDpiAwareness();
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	// First thing on screen: the tray icon, before any config or desktop warnings can pop up.
	trayInit(instance, trayHandler);

	wchar_t problems[2048];
	configLoad(&config, problems, 2048);

	wchar_t desktopProblem[512];
	vdInit(config.desktopBackend, desktopProblem, 512);
	lastDesktop = vdCurrent();
	refreshTray();

	tilingInit(&config);
	tilingSetScheduler(scheduleRetile);

	keyboardInit(&config);

	wchar_t keyProblems[2048];
	keyboardRegister(keyProblems, 2048);
	wcsncat(problems, keyProblems, 2048 - wcslen(problems) - 1);

	if (!altdragInstall(&config)) {
		wcsncat(problems, L"Could not install the mouse/keyboard hooks needed for altdrag.\n", 2048 - wcslen(problems) - 1);
	}

	installEventHooks();
	hookRefreshTimer = SetTimer(NULL, 0, HOOK_REFRESH_MS, NULL);

	if (desktopProblem[0]) {
		wcsncat(problems, desktopProblem, 2048 - wcslen(problems) - 1);
	}
	showProblems(problems);

	tilingRetile();

	MSG msg;
	while (running && GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_LWM_ACTION || msg.message == WM_HOTKEY) {
			const Binding* binding = keyboardLookup(msg.wParam);
			if (binding && !keyboardIsDuplicate(msg.wParam)) runAction(binding);
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && msg.wParam == hookRefreshTimer) {
			altdragRefreshHooks();
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && msg.wParam == retileTimer) {
			KillTimer(NULL, retileTimer);
			retileTimer = 0;
			tilingRetile();
		} else {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	for (int i = 0; i < EVENT_HOOK_COUNT; i++) {
		if (eventHooks[i]) UnhookWinEvent(eventHooks[i]);
	}

	altdragUninstall();
	keyboardUnregister();
	tilingReload(&config);
	trayCleanup();
	vdCleanup();
	configFree(&config);
	CoUninitialize();
	CloseHandle(mutex);

	return EXIT_OK;
}
