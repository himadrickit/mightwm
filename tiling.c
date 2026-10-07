#include "tiling.h"
#include "layout.h"
#include "vdesktop.h"
#include <dwmapi.h>
#include <limits.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef EVENT_OBJECT_DESTROY
#define EVENT_OBJECT_DESTROY 0x8001
#endif

#define MAX_WINDOWS 512
#define MONITOR_SLOTS 16

typedef enum { MODE_TILE, MODE_MONOCLE, MODE_FULLSCREEN } Mode;

typedef struct {
	HMONITOR handle;
	RECT work;
	RECT full;
} MonitorSlot;

static const Config* config = NULL;
static HWND order[MAX_WINDOWS];
static int orderCount = 0;
static HWND floated[MAX_WINDOWS];
static int floatedCount = 0;
static bool enabled = true;
static bool suspended = false;
static Mode mode = MODE_TILE;
static HWND modeTarget = NULL;
static HWND topmostWindow = NULL;
static HWND borderlessWindow = NULL;
static LONG_PTR savedStyle = 0;
static int masterPercent = 50;
static MonitorSlot monitors[MONITOR_SLOTS];
static int monitorCount = 0;

// Windows that are never worth tiling: shell surfaces, flyouts and tray overflow hosts.
static const wchar_t* const systemClasses[] = {
	L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
	L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
	L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
	L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskListThumbnailWnd",
	L"TaskListOverlayWnd", L"Shell_InputSwitchTopLevelWindow", L"SysShadow"
};

static int indexOf(HWND* array, int count, HWND window)
{
	for (int i = 0; i < count; i++) {
		if (array[i] == window) return i;
	}
	return -1;
}

static void removeAt(HWND* array, int* count, int index)
{
	for (int i = index; i < *count - 1; i++) {
		array[i] = array[i + 1];
	}
	(*count)--;
}

static LRect toLRect(RECT r)
{
	LRect result = { r.left, r.top, r.right, r.bottom };
	return result;
}

// The bounds of the window as the user sees it (without the invisible resize borders).
static void getFrame(HWND window, RECT* frame)
{
	if (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, frame, sizeof(RECT)) != S_OK) {
		GetWindowRect(window, frame);
	}
}

static bool getExeName(HWND window, wchar_t* out, DWORD count)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	if (!pid) return false;

	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return false;

	wchar_t path[MAX_PATH];
	DWORD length = MAX_PATH;
	BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
	CloseHandle(process);
	if (!ok) return false;

	const wchar_t* base = wcsrchr(path, L'\\');
	wcsncpy(out, base ? base + 1 : path, count - 1);
	out[count - 1] = 0;
	return true;
}

static bool matchesFloatRule(HWND window, const wchar_t* className)
{
	if (config->floatRuleCount == 0) return false;

	wchar_t exe[MAX_PATH];
	bool haveExe = getExeName(window, exe, MAX_PATH);

	for (int i = 0; i < config->floatRuleCount; i++) {
		if (_wcsicmp(className, config->floatRules[i]) == 0) return true;
		if (haveExe && _wcsicmp(exe, config->floatRules[i]) == 0) return true;
	}
	return false;
}

// Structural test: could this window ever be tiled? Cheap checks run first.
static bool isCandidate(HWND window)
{
	if (!IsWindow(window) || !IsWindowVisible(window)) return false;
	if (GetAncestor(window, GA_ROOT) != window) return false;
	if (GetWindow(window, GW_OWNER) != NULL) return false;

	LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
	LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);

	if (style & WS_CHILD) return false;
	if ((exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !(exStyle & WS_EX_APPWINDOW)) return false;

	// Only resizable windows can be tiled. This also drops flyouts, menus, tooltips and dialogs.
	if (!(style & WS_THICKFRAME)) return false;
	if (GetWindowTextLengthW(window) == 0) return false;

	wchar_t className[256];
	if (!GetClassNameW(window, className, 256)) return false;

	for (size_t i = 0; i < sizeof systemClasses / sizeof systemClasses[0]; i++) {
		if (wcscmp(className, systemClasses[i]) == 0) return false;
	}

	return !matchesFloatRule(window, className);
}

static bool isCloaked(HWND window)
{
	DWORD cloaked = 0;
	return DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked) == S_OK && cloaked != 0;
}

// On screen right now: not hidden, minimized, cloaked, or living on another virtual desktop.
static bool isShownNow(HWND window)
{
	if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || isCloaked(window)) return false;
	return vdWindowOnCurrent(window);
}

static bool isTileableNow(HWND window)
{
	return isShownNow(window) && indexOf(floated, floatedCount, window) < 0;
}

static BOOL CALLBACK collectProc(HWND window, LPARAM unused)
{
	if (orderCount >= MAX_WINDOWS) return FALSE;

	if (indexOf(order, orderCount, window) < 0 && isCandidate(window)) {
		order[orderCount++] = window;
	}
	return TRUE;
}

// Keeps `order` in sync with reality: forgets dead windows, appends new ones (top of z-order first).
static void syncWindows(void)
{
	for (int i = orderCount - 1; i >= 0; i--) {
		if (!IsWindow(order[i])) removeAt(order, &orderCount, i);
	}
	for (int i = floatedCount - 1; i >= 0; i--) {
		if (!IsWindow(floated[i])) removeAt(floated, &floatedCount, i);
	}

	EnumWindows(collectProc, 0);
}

static int collectTileable(HWND* out)
{
	int count = 0;
	for (int i = 0; i < orderCount; i++) {
		if (isTileableNow(order[i])) out[count++] = order[i];
	}
	return count;
}

static BOOL CALLBACK monitorProc(HMONITOR handle, HDC dc, LPRECT rect, LPARAM unused)
{
	MONITORINFO info;
	info.cbSize = sizeof info;

	if (monitorCount < MONITOR_SLOTS && GetMonitorInfoW(handle, &info)) {
		monitors[monitorCount].handle = handle;
		monitors[monitorCount].work = info.rcWork;
		monitors[monitorCount].full = info.rcMonitor;
		monitorCount++;
	}
	return TRUE;
}

static void refreshMonitors(void)
{
	monitorCount = 0;
	EnumDisplayMonitors(NULL, NULL, monitorProc, 0);
}

static int groupOnMonitor(HWND* list, int count, HMONITOR monitor, HWND* out)
{
	int grouped = 0;
	for (int i = 0; i < count; i++) {
		if (MonitorFromWindow(list[i], MONITOR_DEFAULTTONEAREST) == monitor) out[grouped++] = list[i];
	}
	return grouped;
}

// Puts the visible frame of `window` exactly on `target`, compensating for the invisible borders
// Windows 10/11 add around resizable windows (which is what makes naive tiling leave gaps).
static void placeWindow(HWND window, LRect target, HWND insertAfter)
{
	if (IsZoomed(window)) {
		ShowWindow(window, SW_RESTORE);
	}

	RECT outer, frame;
	GetWindowRect(window, &outer);
	getFrame(window, &frame);

	UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
	if (!insertAfter) flags |= SWP_NOZORDER;

	bool same = frame.left == target.left && frame.top == target.top &&
		frame.right == target.right && frame.bottom == target.bottom;

	if (same) {
		if (!insertAfter) return;
		flags |= SWP_NOMOVE | SWP_NOSIZE;
	}

	int leftBorder = frame.left - outer.left;
	int topBorder = frame.top - outer.top;
	int rightBorder = outer.right - frame.right;
	int bottomBorder = outer.bottom - frame.bottom;

	SetWindowPos(window, insertAfter,
		target.left - leftBorder,
		target.top - topBorder,
		(target.right - target.left) + leftBorder + rightBorder,
		(target.bottom - target.top) + topBorder + bottomBorder,
		flags);
}

static void leaveBorderless(void)
{
	if (borderlessWindow && IsWindow(borderlessWindow)) {
		SetWindowLongPtrW(borderlessWindow, GWL_STYLE, savedStyle);
		SetWindowPos(borderlessWindow, HWND_NOTOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}
	borderlessWindow = NULL;
}

static void releaseTopmost(HWND except)
{
	if (topmostWindow && topmostWindow != except && IsWindow(topmostWindow)) {
		SetWindowPos(topmostWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (topmostWindow != except) topmostWindow = NULL;
}

// Real fullscreen: drop the title bar and borders, cover the whole monitor (taskbar included)
// and stay on top. Everything is put back when fullscreen ends.
static void applyFullscreen(HWND window, RECT monitor)
{
	if (borderlessWindow != window) {
		leaveBorderless();

		if (IsZoomed(window)) ShowWindow(window, SW_RESTORE);

		savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
		SetWindowLongPtrW(window, GWL_STYLE, savedStyle & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME));
		borderlessWindow = window;
	}

	SetWindowPos(window, HWND_TOPMOST, monitor.left, monitor.top,
		monitor.right - monitor.left, monitor.bottom - monitor.top,
		SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	topmostWindow = window;
}

static void applySingleWindowMode(HWND* list, int count)
{
	HWND foreground = GetForegroundWindow();

	if (indexOf(list, count, foreground) >= 0) {
		modeTarget = foreground;
	} else if (indexOf(list, count, modeTarget) < 0) {
		modeTarget = count ? list[0] : NULL;
	}

	if (!modeTarget) return;

	MONITORINFO info;
	info.cbSize = sizeof info;
	GetMonitorInfoW(MonitorFromWindow(modeTarget, MONITOR_DEFAULTTONEAREST), &info);

	leaveBorderless();
	releaseTopmost(modeTarget);
	placeWindow(modeTarget, layoutInner(toLRect(info.rcWork), config->gap), HWND_TOP);
}

void tilingRetile(void)
{
	if (!enabled || suspended || !config) return;

	syncWindows();

	HWND list[MAX_WINDOWS];
	int count = collectTileable(list);

	// Fullscreen belongs to one specific window, tiled or not. It ends when that window leaves
	// this desktop (or is minimized/closed), so the desktop you switched to tiles normally.
	if (mode == MODE_FULLSCREEN) {
		if (borderlessWindow && isShownNow(borderlessWindow)) {
			MONITORINFO info;
			info.cbSize = sizeof info;
			GetMonitorInfoW(MonitorFromWindow(borderlessWindow, MONITOR_DEFAULTTONEAREST), &info);
			applyFullscreen(borderlessWindow, info.rcMonitor);
			return;
		}

		leaveBorderless();
		releaseTopmost(NULL);
		mode = MODE_TILE;
	}

	if (mode != MODE_TILE) {
		applySingleWindowMode(list, count);
		return;
	}

	refreshMonitors();

	for (int m = 0; m < monitorCount; m++) {
		HWND group[MAX_WINDOWS];
		int grouped = groupOnMonitor(list, count, monitors[m].handle, group);
		if (grouped == 0) continue;

		LRect rects[MAX_WINDOWS];
		layoutCompute(config->layout, toLRect(monitors[m].work), grouped, config->gap, masterPercent, rects);

		for (int i = 0; i < grouped; i++) {
			placeWindow(group[i], rects[i], NULL);
		}
	}
}

void tilingInit(const Config* cfg)
{
	config = cfg;
	masterPercent = cfg->masterPercent;
}

void tilingReload(const Config* cfg)
{
	tilingInit(cfg);
	leaveBorderless();
	releaseTopmost(NULL);
}

bool tilingWantsEvent(DWORD event, HWND window)
{
	if (!enabled || suspended || !config) return false;

	if (window == borderlessWindow || indexOf(order, orderCount, window) >= 0) return true;
	if (event == EVENT_OBJECT_DESTROY) return false;
	return isCandidate(window);
}

bool tilingIsCandidate(HWND window)
{
	return config && isCandidate(window);
}

bool tilingIsTiled(HWND window)
{
	return enabled && mode == MODE_TILE && indexOf(order, orderCount, window) >= 0 && isTileableNow(window);
}

bool tilingModeActive(void)
{
	return mode != MODE_TILE;
}

void tilingSetSuspended(bool value)
{
	suspended = value;
}

static void focusWindow(HWND window)
{
	if (IsIconic(window)) ShowWindow(window, SW_RESTORE);

	if (!SetForegroundWindow(window)) {
		SwitchToThisWindow(window, FALSE);
	}

	if (config && config->warpCursor) {
		RECT frame;
		getFrame(window, &frame);
		SetCursorPos((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
	}
}

void tilingFocusRelative(int step)
{
	syncWindows();

	HWND list[MAX_WINDOWS];
	int count = collectTileable(list);
	if (count == 0) return;

	int current = indexOf(list, count, GetForegroundWindow());
	int next = current < 0 ? (step > 0 ? 0 : count - 1) : (current + step + count) % count;

	focusWindow(list[next]);
}

// Picks the closest window in a direction: distance along the axis plus a penalty for being off-axis.
static HWND findNeighbor(HWND from, Direction direction, HWND* list, int count)
{
	RECT fromFrame;
	getFrame(from, &fromFrame);
	long fromX = (fromFrame.left + fromFrame.right) / 2;
	long fromY = (fromFrame.top + fromFrame.bottom) / 2;

	HWND best = NULL;
	long long bestScore = LLONG_MAX;

	for (int i = 0; i < count; i++) {
		if (list[i] == from) continue;

		RECT frame;
		getFrame(list[i], &frame);
		long dx = (frame.left + frame.right) / 2 - fromX;
		long dy = (frame.top + frame.bottom) / 2 - fromY;
		long primary, secondary;

		switch (direction) {
			case DIR_LEFT: primary = -dx; secondary = labs(dy); break;
			case DIR_RIGHT: primary = dx; secondary = labs(dy); break;
			case DIR_UP: primary = -dy; secondary = labs(dx); break;
			default: primary = dy; secondary = labs(dx); break;
		}

		if (primary <= 0) continue;

		long long score = primary + 2LL * secondary;
		if (score < bestScore) {
			bestScore = score;
			best = list[i];
		}
	}

	return best;
}

static HWND currentWindowIn(HWND* list, int count)
{
	HWND foreground = GetForegroundWindow();
	return indexOf(list, count, foreground) >= 0 ? foreground : (count ? list[0] : NULL);
}

void tilingFocusDirection(Direction direction)
{
	syncWindows();

	HWND list[MAX_WINDOWS];
	int count = collectTileable(list);
	if (count == 0) return;

	HWND current = currentWindowIn(list, count);
	HWND target = findNeighbor(current, direction, list, count);

	focusWindow(target ? target : current);
}

void tilingMoveDirection(Direction direction)
{
	if (mode != MODE_TILE) return;

	syncWindows();

	HWND list[MAX_WINDOWS];
	int count = collectTileable(list);
	HWND current = GetForegroundWindow();

	if (indexOf(list, count, current) < 0) return;

	HWND target = findNeighbor(current, direction, list, count);
	if (!target) return;

	int a = indexOf(order, orderCount, current);
	int b = indexOf(order, orderCount, target);
	if (a < 0 || b < 0) return;

	HWND swap = order[a];
	order[a] = order[b];
	order[b] = swap;

	tilingRetile();
}

void tilingToggleFloating(void)
{
	syncWindows();

	HWND window = GetForegroundWindow();
	int index = indexOf(floated, floatedCount, window);

	if (index >= 0) {
		removeAt(floated, &floatedCount, index);
	} else if (indexOf(order, orderCount, window) >= 0 && floatedCount < MAX_WINDOWS) {
		floated[floatedCount++] = window;
	} else {
		return;
	}

	tilingRetile();
}

static void setMode(Mode requested)
{
	if (mode == requested) {
		mode = MODE_TILE;
		leaveBorderless();
		releaseTopmost(NULL);
	} else {
		leaveBorderless();
		releaseTopmost(NULL);
		mode = requested;
		modeTarget = GetForegroundWindow();
	}

	tilingRetile();
}

void tilingToggleMonocle(void) { setMode(MODE_MONOCLE); }

static bool isShellSurface(HWND window)
{
	wchar_t className[64];
	if (!window || !GetClassNameW(window, className, 64)) return true;

	return window == GetDesktopWindow() || window == GetShellWindow() ||
		wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
		wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

// Fullscreen works on whatever window has focus: tiled, floating, not resizable, even with tiling off.
void tilingToggleFullscreen(void)
{
	if (mode == MODE_FULLSCREEN) {
		leaveBorderless();
		releaseTopmost(NULL);
		mode = MODE_TILE;
		tilingRetile();
		return;
	}

	HWND target = GetAncestor(GetForegroundWindow(), GA_ROOT);
	if (!target || isShellSurface(target)) return;

	leaveBorderless();
	releaseTopmost(NULL);

	MONITORINFO info;
	info.cbSize = sizeof info;
	GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &info);

	mode = MODE_FULLSCREEN;
	modeTarget = target;
	applyFullscreen(target, info.rcMonitor);
}

void tilingToggleEnabled(void)
{
	enabled = !enabled;
	if (enabled) tilingRetile();
}

void tilingCloseWindow(void)
{
	HWND window = GetForegroundWindow();
	if (window && isCandidate(window)) {
		PostMessageW(window, WM_CLOSE, 0, 0);
	}
}

// Dropping a dragged tile on top of another one swaps their places; anywhere else snaps it back.
void tilingDragDrop(HWND window, POINT cursor)
{
	if (mode == MODE_TILE && enabled) {
		HWND list[MAX_WINDOWS];
		int count = collectTileable(list);
		int a = indexOf(order, orderCount, window);

		for (int i = 0; i < count && a >= 0; i++) {
			RECT frame;
			if (list[i] == window) continue;

			getFrame(list[i], &frame);
			if (PtInRect(&frame, cursor)) {
				int b = indexOf(order, orderCount, list[i]);
				if (b >= 0) {
					HWND swap = order[a];
					order[a] = order[b];
					order[b] = swap;
				}
				break;
			}
		}
	}

	tilingRetile();
}

// Resizing the master/stack divider with the mouse changes the master width for good.
void tilingResizeDrop(HWND window, RECT frame, int edgeX)
{
	if (mode == MODE_TILE && enabled && edgeX != 0 && config->layout == LAYOUT_MASTER_STACK) {
		HWND list[MAX_WINDOWS], group[MAX_WINDOWS];
		int count = collectTileable(list);

		HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
		int grouped = groupOnMonitor(list, count, monitor, group);
		int index = indexOf(group, grouped, window);

		MONITORINFO info;
		info.cbSize = sizeof info;

		if (grouped >= 2 && index >= 0 && GetMonitorInfoW(monitor, &info)) {
			LRect area = toLRect(info.rcWork);

			if (index == 0 && edgeX > 0) {
				masterPercent = layoutPercentFromDivider(area, config->gap, frame.right);
			} else if (index > 0 && edgeX < 0) {
				masterPercent = layoutPercentFromDivider(area, config->gap, frame.left - config->gap);
			}
		}
	}

	tilingRetile();
}
