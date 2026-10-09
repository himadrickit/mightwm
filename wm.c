#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include "altdrag.h"
#include "config.h"
#include "error.h"
#include "keyboard.h"
#include "log.h"
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
static bool pendingAutoFocus = false;
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

// ---- Workspace focus ------------------------------------------------------------------------
// After a switch, the workspace's remembered window gets focus. Attempts are verified and
// retried, abandoned if you already moved on, and never aimed at a window that is not on the
// current desktop - the cases that made Windows flash taskbar buttons instead of focusing.
#define FOCUS_VERIFY_MS 70
#define FOCUS_BURST_MS 200
#define FOCUS_MAX_ATTEMPTS 4

static int focusDesktop = 0;
static int focusAttempts = 0;
static UINT_PTR focusTimer = 0;

static void attemptWorkspaceFocus(void)
{
	int desktop = focusDesktop;

	if (desktop == 0) return;
	if (config.autoFocus == AUTOFOCUS_OFF) {
		focusDesktop = 0;
		return;
	}

	int current = vdCurrent();
	if (current != desktop) {
		logWrite("focus: workspace %d abandoned, now on %d", desktop, current);
		focusDesktop = 0;
		return;
	}

	HWND target = tilingWorkspaceFocusTarget(desktop);
	if (!target) {
		logWrite("focus: workspace %d has no window to focus", desktop);
		focusDesktop = 0;
		return;
	}

	if (tilingIsForeground(target)) {
		if (logEnabled()) {
			char text[320];
			tilingDescribe(target, text, sizeof text);
			logWrite("focus: workspace %d ok after %d attempt(s): %s", desktop, focusAttempts, text);
			tilingLogFocusTable();
		}
		tilingClearAlert(target);
		focusDesktop = 0;
		return;
	}

	if (focusAttempts >= FOCUS_MAX_ATTEMPTS) {
		logWrite("focus: gave up on workspace %d after %d attempts", desktop, focusAttempts);
		tilingClearAlert(target);
		tilingLogFocusTable();
		focusDesktop = 0;
		return;
	}

	focusAttempts++;
	if (logEnabled()) {
		char text[320];
		tilingDescribe(target, text, sizeof text);
		logWrite("focus: workspace %d attempt %d -> %s", desktop, focusAttempts, text);
	}

	tilingFocusWindow(target);
	focusTimer = SetTimer(NULL, 0, FOCUS_VERIFY_MS, NULL);   // verify, retry if it did not stick
}

static void requestWorkspaceFocus(int desktop, bool immediate)
{
	if (focusTimer) {
		KillTimer(NULL, focusTimer);
		focusTimer = 0;
	}

	focusDesktop = desktop;
	focusAttempts = 0;

	if (immediate) {
		attemptWorkspaceFocus();
	} else {
		focusTimer = SetTimer(NULL, 0, FOCUS_VERIFY_MS, NULL);
	}
}

static void switchDesktop(int number)
{
	if (!vdCanControl() || number < 1) return;

	// Pressing alt+1, alt+2, alt+3 quickly: only the workspace you end on gets focus, so
	// nothing is focused while another switch is already on its way.
	DWORD now = GetTickCount();
	bool burst = now - lastSwitchTick < FOCUS_BURST_MS;
	lastSwitchTick = now;

	vdGoto(number);
	lastDesktop = number;
	logWrite("switch: workspace %d%s", number, burst ? " (burst)" : "");

	requestWorkspaceFocus(number, !burst);
	scheduleRetile(RETILE_DELAY_MS * 2);
	refreshTray();
}

// Sends the focused window to another workspace. How you end up depends on `type`:
//   stay:           you remain here, this workspace's main window takes focus
//   follow:         you go along, the moved window stays focused
//   follow-main:    you go along, the destination's existing main window takes focus
//   follow-promote: you go along, the moved window becomes the destination's main window
// A window that arrives joins the end of the stack unless it is promoted.
static void sendWindow(int number, int type)
{
	HWND window = GetAncestor(GetForegroundWindow(), GA_ROOT);
	if (!window || !vdCanControl() || !tilingIsCandidate(window)) return;

	if (!vdMoveWindow(window, number)) {
		logWrite("send: could not move %p to workspace %d", (void*)window, number);
		return;
	}

	static const char* const names[] = { "stay", "follow", "follow-main", "follow-promote" };
	logWrite("send: %p -> workspace %d (%s)", (void*)window, number, names[type]);

	tilingPlaceInOrder(window, type == MOVE_FOLLOW_PROMOTE);

	switch (type) {
		case MOVE_FOLLOW:
		case MOVE_FOLLOW_PROMOTE:
			tilingRememberFocus(window, number);
			switchDesktop(number);
			break;
		case MOVE_FOLLOW_MAIN:
			tilingForgetFocus(number);   // no remembered window: the main window gets focus
			switchDesktop(number);
			break;
		default:
			pendingAutoFocus = true;
			scheduleRetile(RETILE_DELAY_MS);
			break;
	}
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

// Another Alt+drag tool (AltDrag, AltSnap) hooks the mouse in front of LightWM and swallows
// the clicks, so LightWM's own altdrag never sees them. Say so instead of failing silently.
static void detectAltDragConflict(wchar_t* message, size_t count)
{
	message[0] = 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	PROCESSENTRY32W entry;
	entry.dwSize = sizeof entry;

	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		wchar_t lower[MAX_PATH];
		wcsncpy(lower, entry.szExeFile, MAX_PATH - 1);
		lower[MAX_PATH - 1] = 0;
		_wcslwr(lower);

		if (wcsstr(lower, L"altdrag") || wcsstr(lower, L"altsnap")) {
			swprintf(message, count,
				L"%ls is running. It also handles Alt+mouse-drag and grabs the clicks before LightWM, "
				L"so LightWM's own altdrag (move, resize, swap tiles) cannot work. Quit it to use LightWM's.\n",
				entry.szExeFile);
			logWrite("conflict: %ls is running", entry.szExeFile);
			break;
		}
	}

	CloseHandle(snapshot);
}

static void reloadConfig(void)
{
	wchar_t problems[2048];

	keyboardUnregister();
	configFree(&config);
	configLoad(&config, problems, 2048);
	logInit(config.debugLog);
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
		case TRAY_OPEN_LOG: ShellExecuteW(NULL, L"open", logPath(), NULL, NULL, SW_SHOWNORMAL); break;
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
		case ACT_FOCUS_MASTER: tilingFocusMaster(); break;
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
		case ACT_SEND: sendWindow(b->arg, config.moveType); break;
		case ACT_SEND_FOLLOW: sendWindow(b->arg, config.moveType >= MOVE_FOLLOW ? config.moveType : MOVE_FOLLOW); break;
		case ACT_SEND_STAY: sendWindow(b->arg, MOVE_STAY); break;
		case ACT_MASTER_CYCLE: tilingCycleMaster(); break;
		case ACT_MASTER_GROW: tilingAdjustMaster(5); break;
		case ACT_MASTER_SHRINK: tilingAdjustMaster(-5); break;
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
		// The desktop changed without us (Windows shortcut, taskbar, touchpad gesture).
		lastDesktop = current;
		refreshTray();
		logWrite("switch: workspace %d (external)", current);
		requestWorkspaceFocus(current, false);
		scheduleRetile(RETILE_DELAY_MS * 2);
		return;
	}

	// Remember what has focus on this workspace, so coming back restores it.
	HWND root = GetAncestor(window, GA_ROOT);
	if (root && current > 0 && vdWindowOnCurrent(root) && settled) {
		int desktop = vdWindowDesktop(root);
		if (desktop == 0 || desktop == current) {
			tilingRememberFocus(root, current);
		}
	}

	if (tilingModeActive()) {
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
		bool closing = event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE ||
			event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_OBJECT_CLOAKED;
		if (closing) pendingAutoFocus = true;
		scheduleRetile(closing ? RETILE_DELAY_MS * 3 : RETILE_DELAY_MS);
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
	logInit(config.debugLog);

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

	if (config.altdragEnabled) {
		wchar_t conflict[512];
		detectAltDragConflict(conflict, 512);
		wcsncat(problems, conflict, 2048 - wcslen(problems) - 1);
	}
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
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && focusTimer && msg.wParam == focusTimer) {
			KillTimer(NULL, focusTimer);
			focusTimer = 0;
			attemptWorkspaceFocus();
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && altdragHandleTimer(msg.wParam)) {
			// a throttled drag update was applied
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && msg.wParam == hookRefreshTimer) {
			altdragRefreshHooks();
		} else if (msg.message == WM_TIMER && msg.hwnd == NULL && msg.wParam == retileTimer) {
			KillTimer(NULL, retileTimer);
			retileTimer = 0;
			tilingRetile();

			if (pendingAutoFocus) {
				pendingAutoFocus = false;
				tilingAutoFocus();
			}
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
	logClose();
	vdCleanup();
	configFree(&config);
	CoUninitialize();
	CloseHandle(mutex);

	return EXIT_OK;
}
