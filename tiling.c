#include "tiling.h"
#include "layout.h"
#include "log.h"
#include "vdesktop.h"
#include <dwmapi.h>
#include <limits.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

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
static TilingScheduler scheduler = NULL;
static HWND nativeWindow = NULL;
static RECT nativeStart;
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


// ---- Invisible border insets -------------------------------------------------------------
// Windows 10/11 give resizable windows invisible borders. Their size is a property of the window
// (and its DPI), not of where it is, so it is measured once while the window is at rest and
// cached. Measuring right after a move is wrong: DWM reports the frame a moment late, so the
// "border" would include the distance the window just moved.

#define INSET_CACHE 256

typedef struct {
	HWND window;
	UINT dpi;
	int left, top, right, bottom;
} InsetEntry;

static InsetEntry insetCache[INSET_CACHE];
static int insetCount = 0;
static int insetNext = 0;

static UINT windowDpi(HWND window)
{
	typedef UINT (WINAPI *FnGetDpiForWindow)(HWND);
	static FnGetDpiForWindow fn = NULL;
	static bool loaded = false;

	if (!loaded) {
		fn = (FnGetDpiForWindow)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		loaded = true;
	}

	UINT dpi = fn ? fn(window) : 0;
	return dpi ? dpi : 96;
}

static void forgetDeadInsets(void)
{
	for (int i = insetCount - 1; i >= 0; i--) {
		if (!IsWindow(insetCache[i].window)) {
			insetCache[i] = insetCache[--insetCount];
		}
	}
	if (insetNext >= INSET_CACHE) insetNext = 0;
}

void tilingGetInsets(HWND window, int* left, int* top, int* right, int* bottom)
{
	UINT dpi = windowDpi(window);

	for (int i = 0; i < insetCount; i++) {
		if (insetCache[i].window == window && insetCache[i].dpi == dpi) {
			*left = insetCache[i].left;
			*top = insetCache[i].top;
			*right = insetCache[i].right;
			*bottom = insetCache[i].bottom;
			return;
		}
	}

	RECT outer, frame;
	GetWindowRect(window, &outer);
	getFrame(window, &frame);

	int l = frame.left - outer.left, t = frame.top - outer.top;
	int r = outer.right - frame.right, b = outer.bottom - frame.bottom;
	int limit = (int)(32 * dpi / 96);

	// A real frame is symmetric left/right and small. Anything else is a transient reading
	// (maximized, mid-move) and is not trusted or cached.
	bool plausible = !IsZoomed(window) && l >= 0 && t >= 0 && r >= 0 && b >= 0 &&
		l <= limit && t <= limit && r <= limit && b <= limit && labs(l - r) <= 2;

	if (!plausible) {
		*left = *right = *bottom = (int)(7 * dpi / 96);
		*top = 0;
		return;
	}

	InsetEntry* entry = NULL;
	for (int i = 0; i < insetCount; i++) {
		if (insetCache[i].window == window) entry = &insetCache[i];
	}
	if (!entry) {
		if (insetCount < INSET_CACHE) entry = &insetCache[insetCount++];
		else entry = &insetCache[insetNext++ % INSET_CACHE];
	}

	entry->window = window;
	entry->dpi = dpi;
	entry->left = l;
	entry->top = t;
	entry->right = r;
	entry->bottom = b;

	*left = l;
	*top = t;
	*right = r;
	*bottom = b;
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

	forgetDeadInsets();
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
// Windows that were just moved by a drag. Their GetWindowRect can still show a position from
// before the queued (asynchronous) moves landed, so they are always re-applied once.
#define DIRTY_SLOTS 8
static HWND dirtyWindows[DIRTY_SLOTS];

static void markDirty(HWND window)
{
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (dirtyWindows[i] == window) return;
	}
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (!dirtyWindows[i]) {
			dirtyWindows[i] = window;
			return;
		}
	}
	dirtyWindows[0] = window;
}

static bool takeDirty(HWND window)
{
	for (int i = 0; i < DIRTY_SLOTS; i++) {
		if (dirtyWindows[i] == window) {
			dirtyWindows[i] = NULL;
			return true;
		}
	}
	return false;
}

static void placeWindow(HWND window, LRect target, HWND insertAfter)
{
	if (IsZoomed(window)) {
		ShowWindow(window, SW_RESTORE);
	}

	UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
	if (!insertAfter) flags |= SWP_NOZORDER;

	int leftInset, topInset, rightInset, bottomInset;
	tilingGetInsets(window, &leftInset, &topInset, &rightInset, &bottomInset);

	int x = target.left - leftInset;
	int y = target.top - topInset;
	int width = (target.right - target.left) + leftInset + rightInset;
	int height = (target.bottom - target.top) + topInset + bottomInset;

	// Re-applying every window on every retile made desktop switches and bursts of events
	// janky (every app re-lays itself out). Tiles already exactly in place are left alone.
	bool dirty = takeDirty(window);
	if (!dirty && !insertAfter) {
		RECT outer;
		GetWindowRect(window, &outer);
		if (outer.left == x && outer.top == y && outer.right - outer.left == width && outer.bottom - outer.top == height) {
			return;
		}
	}

	SetWindowPos(window, insertAfter, x, y, width, height, flags);
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

void tilingSetScheduler(TilingScheduler value)
{
	scheduler = value;
}

bool tilingIsEnabled(void)
{
	return enabled;
}

// A second pass shortly after drops lets the windows settle and corrects any app that
// adjusted its own size in the meantime.
static void followUpRetile(void)
{
	if (scheduler) scheduler(150);
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

static bool isShellSurface(HWND window);

// Windows refuses SetForegroundWindow from a background process and flashes the taskbar button
// instead. Briefly sharing input state with the current foreground thread is the standard way
// window managers get around that.
static void forceForeground(HWND window)
{
	HWND foreground = GetForegroundWindow();
	if (foreground == window) return;

	DWORD me = GetCurrentThreadId();
	DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
	DWORD targetThread = GetWindowThreadProcessId(window, NULL);

	bool attachedForeground = foregroundThread && foregroundThread != me && AttachThreadInput(me, foregroundThread, TRUE);
	bool attachedTarget = targetThread && targetThread != me && targetThread != foregroundThread && AttachThreadInput(me, targetThread, TRUE);

	BringWindowToTop(window);
	if (!SetForegroundWindow(window)) {
		SwitchToThisWindow(window, FALSE);
	}

	if (attachedTarget) AttachThreadInput(me, targetThread, FALSE);
	if (attachedForeground) AttachThreadInput(me, foregroundThread, FALSE);

	if (GetForegroundWindow() != window) {
		// Still refused: at least do not leave the button flashing.
		FLASHWINFO flash;
		memset(&flash, 0, sizeof flash);
		flash.cbSize = sizeof flash;
		flash.hwnd = window;
		flash.dwFlags = FLASHW_STOP;
		FlashWindowEx(&flash);
	}
}

static void focusWindow(HWND window)
{
	if (IsIconic(window)) ShowWindow(window, SW_RESTORE);

	forceForeground(window);
	tilingClearAlert(window);

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



// ---- Per-workspace focus memory -------------------------------------------------------------

#define MAX_DESKTOP_SLOTS 64
static HWND focusMemory[MAX_DESKTOP_SLOTS + 1];

void tilingDescribe(HWND window, char* out, size_t size)
{
	wchar_t className[64] = L"?";
	wchar_t title[48] = L"";
	char className8[128] = "?", title8[128] = "";

	if (window && IsWindow(window)) {
		GetClassNameW(window, className, 64);
		GetWindowTextW(window, title, 48);
	}

	WideCharToMultiByte(CP_UTF8, 0, className, -1, className8, sizeof className8, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, title, -1, title8, sizeof title8, NULL, NULL);
	snprintf(out, size, "%p [%s] \"%s\"", (void*)window, className8, title8);
}

void tilingRememberFocus(HWND window, int desktop)
{
	if (desktop < 1 || desktop > MAX_DESKTOP_SLOTS || !window) return;
	if (indexOf(order, orderCount, window) < 0) return;   // only real application windows

	if (focusMemory[desktop] != window) {
		focusMemory[desktop] = window;

		if (logEnabled()) {
			char text[320];
			tilingDescribe(window, text, sizeof text);
			logWrite("focus memory: workspace %d -> %s", desktop, text);
		}
	}
}

void tilingForgetFocus(int desktop)
{
	if (desktop >= 1 && desktop <= MAX_DESKTOP_SLOTS) {
		focusMemory[desktop] = NULL;
	}
}

void tilingPlaceInOrder(HWND window, bool front)
{
	int index = indexOf(order, orderCount, window);
	if (index < 0) return;

	removeAt(order, &orderCount, index);

	if (front) {
		for (int i = orderCount; i > 0; i--) order[i] = order[i - 1];
		order[0] = window;
	} else {
		order[orderCount] = window;
	}
	orderCount++;

	logWrite("order: %p placed at the %s", (void*)window, front ? "front (main window)" : "end of the stack");
}

// A window can only be focused when it exists, is shown and lives on the desktop we are on.
static bool canFocusNow(HWND window)
{
	return window && IsWindow(window) && IsWindowVisible(window) && !IsIconic(window) && vdWindowOnCurrent(window);
}

// What should have focus on this workspace: the window that had it last time, otherwise the master.
HWND tilingWorkspaceFocusTarget(int desktop)
{
	if (desktop >= 1 && desktop <= MAX_DESKTOP_SLOTS && canFocusNow(focusMemory[desktop])) {
		return focusMemory[desktop];
	}

	HWND master = NULL;
	for (int i = 0; i < orderCount && !master; i++) {
		if (canFocusNow(order[i]) && indexOf(floated, floatedCount, order[i]) < 0) master = order[i];
	}
	return master;
}

bool tilingIsForeground(HWND window)
{
	return GetAncestor(GetForegroundWindow(), GA_ROOT) == window;
}

void tilingFocusWindow(HWND window)
{
	if (canFocusNow(window)) {
		focusWindow(window);
	}
}

void tilingClearAlert(HWND window)
{
	FLASHWINFO flash;
	memset(&flash, 0, sizeof flash);
	flash.cbSize = sizeof flash;
	flash.hwnd = window;
	flash.dwFlags = FLASHW_STOP;
	FlashWindowEx(&flash);
}

void tilingLogFocusTable(void)
{
	if (!logEnabled()) return;

	int current = vdCurrent();
	logWrite("focus table (foreground now: see below):");

	for (int d = 1; d <= MAX_DESKTOP_SLOTS; d++) {
		if (!focusMemory[d]) continue;

		char text[320];
		tilingDescribe(focusMemory[d], text, sizeof text);
		logWrite("  workspace %d%s: %s%s", d, d == current ? " (current)" : "", text,
			IsWindow(focusMemory[d]) ? "" : "  [window gone]");
	}

	char text[320];
	tilingDescribe(GetAncestor(GetForegroundWindow(), GA_ROOT), text, sizeof text);
	logWrite("  foreground: %s", text);
}

// The master window is the first tiled window of the current desktop (the left one in
// master-stack); with a single window, that window. Found straight from the known window
// order with cheap checks, so it can run right after a desktop switch.
static HWND findMaster(void)
{
	for (int i = 0; i < orderCount; i++) {
		HWND window = order[i];

		if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) continue;
		if (indexOf(floated, floatedCount, window) >= 0) continue;
		if (!vdWindowOnCurrent(window)) continue;

		return window;
	}
	return NULL;
}

void tilingFocusMaster(void)
{
	HWND master = findMaster();
	if (master) {
		focusWindow(master);
	}
}

void tilingAutoFocus(void)
{
	if (!enabled || suspended || !config || config->autoFocus == AUTOFOCUS_OFF || mode != MODE_TILE) {
		return;
	}

	HWND master = findMaster();
	if (!master) {
		return;
	}

	HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);

	// Never take focus from a window the user floated.
	if (foreground && indexOf(floated, floatedCount, foreground) >= 0) {
		return;
	}

	if (config->autoFocus == AUTOFOCUS_LOST) {
		bool lost = !foreground || !IsWindow(foreground) || !IsWindowVisible(foreground) ||
			isShellSurface(foreground) || !isShownNow(foreground);
		if (!lost) {
			return;
		}
	}

	if (foreground != master) {
		logWrite("autofocus: master=%p (was %p)", (void*)master, (void*)foreground);
		focusWindow(master);
	}
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

// Floating mode: the window leaves the layout, shrinks to a centered floating size on top of
// the tiles (config: float-size), and can then be moved/resized freely. Toggle again to tile it.
void tilingToggleFloating(void)
{
	syncWindows();

	HWND window = GetAncestor(GetForegroundWindow(), GA_ROOT);
	if (!window) return;

	int index = indexOf(floated, floatedCount, window);

	if (index >= 0) {
		removeAt(floated, &floatedCount, index);
		tilingRetile();
		followUpRetile();
		return;
	}

	if (indexOf(order, orderCount, window) < 0 || floatedCount >= MAX_WINDOWS) return;

	floated[floatedCount++] = window;

	if (mode == MODE_TILE && enabled) {
		MONITORINFO info;
		info.cbSize = sizeof info;
		GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);

		int percent = config->floatPercent < 30 ? 30 : config->floatPercent > 100 ? 100 : config->floatPercent;
		int width = (info.rcWork.right - info.rcWork.left) * percent / 100;
		int height = (info.rcWork.bottom - info.rcWork.top) * percent / 100;
		int left = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
		int top = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;

		LRect target = { left, top, left + width, top + height };
		placeWindow(window, target, HWND_TOP);
	}

	tilingRetile();
	followUpRetile();
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

// Grows or shrinks the main window's share of the screen (10-90%) until the config is reloaded.
void tilingAdjustMaster(int deltaPercent)
{
	int updated = masterPercent + deltaPercent;
	masterPercent = updated < 10 ? 10 : updated > 90 ? 90 : updated;
	logWrite("master width: %d%%", masterPercent);
	tilingRetile();
}

// Steps through the `autowidth` presets (for example 65 -> 60 -> 70 -> 65). From a width that is
// not a preset (after growing/shrinking by hand) it goes back to the first preset.
void tilingCycleMaster(void)
{
	if (!config || config->widthPresetCount < 1) return;

	int index = -1;
	for (int i = 0; i < config->widthPresetCount; i++) {
		if (config->widthPresets[i] == masterPercent) index = i;
	}

	masterPercent = config->widthPresets[(index + 1) % config->widthPresetCount];
	logWrite("master width preset: %d%%", masterPercent);
	tilingRetile();
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

// The visible frame of a window computed from its outer rectangle and cached insets
// (GetWindowRect is current immediately; DWM's frame bounds can lag).
static RECT currentFrameOf(HWND window)
{
	RECT outer;
	int left, top, right, bottom;

	GetWindowRect(window, &outer);
	tilingGetInsets(window, &left, &top, &right, &bottom);

	RECT frame = { outer.left + left, outer.top + top, outer.right - right, outer.bottom - bottom };
	return frame;
}

static long long overlapArea(RECT frame, LRect cell)
{
	long long width = (frame.right < cell.right ? frame.right : cell.right) - (frame.left > cell.left ? frame.left : cell.left);
	long long height = (frame.bottom < cell.bottom ? frame.bottom : cell.bottom) - (frame.top > cell.top ? frame.top : cell.top);
	return (width > 0 && height > 0) ? width * height : 0;
}

// Which tile did the user drop on? Uses the layout's cells (not the live window frames, which
// may still be mid-move): first the cell under the cursor, otherwise the cell the dragged
// window overlaps most. Returns the window that currently sits in that cell.
static HWND windowAtDrop(POINT cursor, RECT frame)
{
	HWND list[MAX_WINDOWS], group[MAX_WINDOWS];
	LRect cells[MAX_WINDOWS];
	int count = collectTileable(list);

	HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
	int grouped = groupOnMonitor(list, count, monitor, group);

	MONITORINFO info;
	info.cbSize = sizeof info;
	if (grouped == 0 || !GetMonitorInfoW(monitor, &info)) {
		return NULL;
	}

	layoutCompute(config->layout, toLRect(info.rcWork), grouped, config->gap, masterPercent, cells);

	int best = -1;
	for (int i = 0; i < grouped; i++) {
		if (cursor.x >= cells[i].left && cursor.x < cells[i].right && cursor.y >= cells[i].top && cursor.y < cells[i].bottom) {
			best = i;
			break;
		}
	}

	if (best < 0) {
		long long bestArea = 0;
		for (int i = 0; i < grouped; i++) {
			long long area = overlapArea(frame, cells[i]);
			if (area > bestArea) {
				bestArea = area;
				best = i;
			}
		}
	}

	logWrite("drop: cursor=%ld,%ld frame=%ld,%ld,%ld,%ld tiles=%d chosen=%d",
		cursor.x, cursor.y, frame.left, frame.top, frame.right, frame.bottom, grouped, best);

	return best >= 0 ? group[best] : NULL;
}

// Dropping a dragged tile on another tile swaps their places; anywhere else snaps it back.
void tilingDragDrop(HWND window, POINT cursor, RECT frame)
{
	markDirty(window);

	if (mode == MODE_TILE && enabled) {
		HWND target = windowAtDrop(cursor, frame);
		int a = indexOf(order, orderCount, window);

		if (target && target != window && a >= 0) {
			int b = indexOf(order, orderCount, target);
			if (b >= 0) {
				HWND swap = order[a];
				order[a] = order[b];
				order[b] = swap;
				logWrite("drop: swapped %p with %p", (void*)window, (void*)target);
			}
		} else {
			logWrite("drop: no swap (target=%p window=%p index=%d)", (void*)target, (void*)window, a);
		}
	} else {
		logWrite("drop: ignored (mode=%d enabled=%d)", (int)mode, (int)enabled);
	}

	tilingRetile();
	followUpRetile();
}

// Resizing the master/stack divider with the mouse changes the master width for good.
void tilingResizeDrop(HWND window, RECT frame, int edgeX)
{
	markDirty(window);

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
	followUpRetile();
}

// Dragging a tiled window by its title bar (Windows' own move/size loop): hold the tiling
// still while it lasts, then treat the drop like an altdrag drop.
void tilingNativeMoveStart(HWND window)
{
	nativeWindow = NULL;

	bool tiled = tilingIsTiled(window);
	logWrite("native move start: window=%p tiled=%d", (void*)window, (int)tiled);
	if (!tiled) {
		return;
	}

	nativeWindow = window;
	getFrame(window, &nativeStart);
	suspended = true;
}

void tilingNativeMoveEnd(HWND window)
{
	if (!nativeWindow || window != nativeWindow) {
		return;
	}

	HWND dropped = nativeWindow;
	nativeWindow = NULL;
	suspended = false;
	logWrite("native move end: window=%p", (void*)dropped);

	RECT end;
	POINT cursor;
	getFrame(dropped, &end);
	GetCursorPos(&cursor);

	const int slack = 3;
	bool resized = labs((end.right - end.left) - (nativeStart.right - nativeStart.left)) > slack ||
		labs((end.bottom - end.top) - (nativeStart.bottom - nativeStart.top)) > slack;

	if (!resized) {
		tilingDragDrop(dropped, cursor, currentFrameOf(dropped));
		return;
	}

	int edgeX = 0;
	bool rightMoved = labs(end.right - nativeStart.right) > slack;
	bool leftMoved = labs(end.left - nativeStart.left) > slack;
	if (rightMoved && !leftMoved) edgeX = 1;
	else if (leftMoved && !rightMoved) edgeX = -1;

	tilingResizeDrop(dropped, end, edgeX);
}
