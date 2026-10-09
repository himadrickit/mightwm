#include "altdrag.h"
#include "keyboard.h"
#include "log.h"
#include "tiling.h"
#include "vdesktop.h"
#include <dwmapi.h>
#include <string.h>
#include <wchar.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

// Marks the key events we inject ourselves so the keyboard hook ignores them.
#define INJECT_MARK ((ULONG_PTR)0x4C574D31)

#define REPEAT_GUARD_MS 1100
#define MIN_WIDTH 120
#define MIN_HEIGHT 80

typedef enum { ACTION_NONE, ACTION_MOVE, ACTION_RESIZE } DragAction;

static const Config* config = NULL;
static HHOOK keyboardHook = NULL;
static HHOOK mouseHook = NULL;

static UINT modsDown = 0;
static bool disguiseModRelease = false;
static bool swallowedKeys[256];
static DWORD swallowedTick[256];
static DWORD ownerThread = 0;

static DragAction action = ACTION_NONE;
static int activeButton = BTN_NONE;
static HWND dragWindow = NULL;
static bool wasTiled = false;
static POINT startPoint;
static RECT startFrame;
static RECT currentFrame;
static int borderLeft, borderTop, borderRight, borderBottom;
static int edgeX, edgeY;

// Mouse moves arrive up to ~1000 times a second. Moving a window that often just queues work
// the app cannot keep up with, so updates are limited to ~144 per second; a short timer
// applies the newest position if the mouse stops in between.
#define MIN_UPDATE_MS 7
static POINT latestPoint;
static bool updatePending = false;
static UINT_PTR flushTimer = 0;
static LARGE_INTEGER counterFrequency, lastUpdate;

static UINT modifierOf(DWORD vk)
{
	switch (vk) {
		case VK_LMENU: case VK_RMENU: case VK_MENU: return MOD_ALT;
		case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return MOD_CONTROL;
		case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return MOD_SHIFT;
		case VK_LWIN: case VK_RWIN: return MOD_WIN;
		default: return 0;
	}
}

// Releasing Alt (or Win) after using it as a modifier would open the window menu (or Start).
// A Ctrl tap in between makes Windows treat it as a used key combination instead.
static void disguiseRelease(void)
{
	INPUT input[2];
	memset(input, 0, sizeof input);

	input[0].type = INPUT_KEYBOARD;
	input[0].ki.wVk = VK_CONTROL;
	input[0].ki.dwExtraInfo = INJECT_MARK;
	input[1] = input[0];
	input[1].ki.dwFlags = KEYEVENTF_KEYUP;

	SendInput(2, input, sizeof(INPUT));
}

static LRESULT CALLBACK keyboardProc(int code, WPARAM wparam, LPARAM lparam)
{
	if (code == HC_ACTION) {
		const KBDLLHOOKSTRUCT* key = (const KBDLLHOOKSTRUCT*)lparam;

		if (key->dwExtraInfo != INJECT_MARK) {
			bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
			bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
			UINT modifier = modifierOf(key->vkCode);

			if (modifier) {
				if (down) {
					UINT before = modsDown;
					modsDown |= modifier;
					if (before != modsDown) {
						logWrite("key: modifier down vk=0x%lX mods=0x%X", (unsigned long)key->vkCode, modsDown);
					}
				} else if (up) {
					logWrite("key: modifier up vk=0x%lX mods=0x%X", (unsigned long)key->vkCode, modsDown);
					if (disguiseModRelease && (modifier == MOD_ALT || modifier == MOD_WIN)) {
						disguiseRelease();
					}
					modsDown &= ~modifier;

					if (!(modsDown & (MOD_ALT | MOD_WIN))) {
						disguiseModRelease = false;
					}
				}
			} else if (key->vkCode < 256) {
				if (down) {
					// Trust the real key state over our bookkeeping, so a missed key-up
					// can never leave a modifier "stuck" and a keybind dead.
					UINT real = 0;
					if (key->flags & LLKHF_ALTDOWN) real |= MOD_ALT;
					if (GetAsyncKeyState(VK_CONTROL) & 0x8000) real |= MOD_CONTROL;
					if (GetAsyncKeyState(VK_SHIFT) & 0x8000) real |= MOD_SHIFT;
					if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) real |= MOD_WIN;
					modsDown = real;

					// Auto-repeat of a key we already handled. The time limit keeps a lost
					// key-up from disabling the bind for good.
					DWORD now = GetTickCount();
					if (swallowedKeys[key->vkCode] && now - swallowedTick[key->vkCode] < REPEAT_GUARD_MS) {
						swallowedTick[key->vkCode] = now;
						return 1;
					}

					int id = keyboardMatch(modsDown, key->vkCode);
					if (id) {
						swallowedKeys[key->vkCode] = true;
						swallowedTick[key->vkCode] = now;
						PostThreadMessageW(ownerThread, WM_LWM_ACTION, (WPARAM)id, 0);
						if (modsDown & (MOD_ALT | MOD_WIN)) disguiseModRelease = true;
						return 1;
					}

					// Windows' own desktop shortcuts (ctrl+win+d, ctrl+win+left/right) are turned off
					// once LightWM can drive the desktops itself; alt+1..9 replaces them.
					if (config->blockWindowsDesktopKeys && vdCanControl() && modsDown == (MOD_CONTROL | MOD_WIN) &&
						(key->vkCode == 'D' || key->vkCode == VK_LEFT || key->vkCode == VK_RIGHT)) {
						swallowedKeys[key->vkCode] = true;
						swallowedTick[key->vkCode] = now;
						disguiseModRelease = true;
						return 1;
					}

					// Some other key was pressed together with Alt/Win (a shortcut, a drag...).
					if (modsDown & (MOD_ALT | MOD_WIN)) disguiseModRelease = true;
				} else if (up && swallowedKeys[key->vkCode]) {
					swallowedKeys[key->vkCode] = false;
					return 1;
				}
			}
		}
	}

	return CallNextHookEx(NULL, code, wparam, lparam);
}

static bool isShellWindow(HWND window)
{
	wchar_t className[64];
	if (!GetClassNameW(window, className, 64)) return true;

	return window == GetDesktopWindow() || window == GetShellWindow() ||
		wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
		wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

static bool beginDrag(POINT point, DragAction wanted, int button)
{
	HWND window = WindowFromPoint(point);
	if (!window) {
		logWrite("altdrag refused: no window under the cursor");
		return false;
	}

	window = GetAncestor(window, GA_ROOT);
	if (!window || isShellWindow(window)) {
		logWrite("altdrag refused: shell or desktop window %p", (void*)window);
		return false;
	}
	if (GetWindowThreadProcessId(window, NULL) == GetCurrentThreadId()) {
		logWrite("altdrag refused: LightWM's own window");
		return false;
	}

	if (IsZoomed(window)) {
		ShowWindow(window, SW_RESTORE);
	}

	RECT outer;
	GetWindowRect(window, &outer);
	tilingGetInsets(window, &borderLeft, &borderTop, &borderRight, &borderBottom);

	startFrame.left = outer.left + borderLeft;
	startFrame.top = outer.top + borderTop;
	startFrame.right = outer.right - borderRight;
	startFrame.bottom = outer.bottom - borderBottom;

	startPoint = point;
	latestPoint = point;
	updatePending = false;
	QueryPerformanceCounter(&lastUpdate);
	currentFrame = startFrame;
	dragWindow = window;
	action = wanted;
	activeButton = button;
	edgeX = edgeY = 0;

	if (wanted == ACTION_RESIZE) {
		// Think of the window as 3x3 boxes: the box you grab decides which edges follow the mouse.
		int width = startFrame.right - startFrame.left;
		int height = startFrame.bottom - startFrame.top;
		int relativeX = point.x - startFrame.left;
		int relativeY = point.y - startFrame.top;

		edgeX = relativeX < width * 38 / 100 ? -1 : relativeX < width * 62 / 100 ? 0 : 1;
		edgeY = relativeY < height * 38 / 100 ? -1 : relativeY < height * 62 / 100 ? 0 : 1;

		if (edgeX == 0 && edgeY == 0) {
			edgeX = relativeX < width / 2 ? -1 : 1;
			edgeY = relativeY < height / 2 ? -1 : 1;
		}
	}

	wasTiled = tilingIsTiled(window);
	logWrite("altdrag start: window=%p tiled=%d action=%d pt=%ld,%ld", (void*)window, (int)wasTiled, (int)wanted, point.x, point.y);
	tilingSetSuspended(true);

	SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SetForegroundWindow(window);
	return true;
}

static void applyDrag(void)
{
	updatePending = false;

	int dx = latestPoint.x - startPoint.x;
	int dy = latestPoint.y - startPoint.y;
	RECT frame = startFrame;
	UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;

	if (action == ACTION_MOVE) {
		OffsetRect(&frame, dx, dy);
		flags |= SWP_NOSIZE;   // a pure move must not make the app re-layout its contents
	} else {
		if (edgeX < 0) frame.left += dx;
		if (edgeX > 0) frame.right += dx;
		if (edgeY < 0) frame.top += dy;
		if (edgeY > 0) frame.bottom += dy;

		if (frame.right - frame.left < MIN_WIDTH) {
			if (edgeX < 0) frame.left = frame.right - MIN_WIDTH;
			else frame.right = frame.left + MIN_WIDTH;
		}
		if (frame.bottom - frame.top < MIN_HEIGHT) {
			if (edgeY < 0) frame.top = frame.bottom - MIN_HEIGHT;
			else frame.bottom = frame.top + MIN_HEIGHT;
		}
	}

	currentFrame = frame;

	SetWindowPos(dragWindow, NULL,
		frame.left - borderLeft,
		frame.top - borderTop,
		(frame.right - frame.left) + borderLeft + borderRight,
		(frame.bottom - frame.top) + borderTop + borderBottom,
		flags);

	QueryPerformanceCounter(&lastUpdate);
}

static void updateDrag(POINT point)
{
	latestPoint = point;
	updatePending = true;

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	long long elapsedMs = (now.QuadPart - lastUpdate.QuadPart) * 1000 / counterFrequency.QuadPart;

	if (elapsedMs >= MIN_UPDATE_MS) {
		applyDrag();
	} else if (!flushTimer) {
		flushTimer = SetTimer(NULL, 0, MIN_UPDATE_MS, NULL);
	}
}

bool altdragHandleTimer(UINT_PTR timerId)
{
	if (!flushTimer || timerId != flushTimer) {
		return false;
	}

	KillTimer(NULL, flushTimer);
	flushTimer = 0;

	if (action != ACTION_NONE && updatePending) {
		applyDrag();
	}
	return true;
}

static void endDrag(POINT point)
{
	if (flushTimer) {
		KillTimer(NULL, flushTimer);
		flushTimer = 0;
	}
	latestPoint = point;
	applyDrag();   // land exactly where the button was released

	DragAction finished = action;
	HWND window = dragWindow;
	bool tiled = wasTiled;

	action = ACTION_NONE;
	activeButton = BTN_NONE;
	dragWindow = NULL;
	tilingSetSuspended(false);

	logWrite("altdrag end: window=%p tiled=%d pt=%ld,%ld", (void*)window, (int)tiled, point.x, point.y);

	if (!tiled) {
		return;
	}

	if (finished == ACTION_MOVE) tilingDragDrop(window, point, currentFrame);
	else tilingResizeDrop(window, currentFrame, edgeX);
}

static int buttonOf(WPARAM message)
{
	switch (message) {
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: return BTN_LEFT;
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: return BTN_RIGHT;
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: return BTN_MIDDLE;
		default: return BTN_NONE;
	}
}

static LRESULT CALLBACK mouseProc(int code, WPARAM wparam, LPARAM lparam)
{
	if (code != HC_ACTION || !config->altdragEnabled) {
		return CallNextHookEx(NULL, code, wparam, lparam);
	}

	const MSLLHOOKSTRUCT* mouse = (const MSLLHOOKSTRUCT*)lparam;
	int button = buttonOf(wparam);
	bool isDown = wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN;
	bool isUp = wparam == WM_LBUTTONUP || wparam == WM_RBUTTONUP || wparam == WM_MBUTTONUP;

	if (isDown || isUp) {
		logWrite("mouse: %s button=%d mods=0x%X action=%d enabled=%d wanted=0x%X",
			isDown ? "down" : "up", button, modsDown, (int)action, (int)config->altdragEnabled, config->altdragMods);
	}

	if (isDown && action == ACTION_NONE && modsDown != 0 && modsDown != config->altdragMods) {
		logWrite("altdrag ignored: modifiers held=0x%X, wanted=0x%X", modsDown, config->altdragMods);
	}

	if (isDown && action == ACTION_NONE && modsDown == config->altdragMods) {
		DragAction wanted = ACTION_NONE;
		if (button == config->altdragMoveButton) wanted = ACTION_MOVE;
		else if (button == config->altdragResizeButton) wanted = ACTION_RESIZE;

		if (wanted != ACTION_NONE && beginDrag(mouse->pt, wanted, button)) {
			disguiseModRelease = true;
			return 1;
		}
	} else if (wparam == WM_MOUSEMOVE && action != ACTION_NONE) {
		updateDrag(mouse->pt);
	} else if (isUp && action != ACTION_NONE && button == activeButton) {
		endDrag(mouse->pt);
		return 1;
	}

	return CallNextHookEx(NULL, code, wparam, lparam);
}

bool altdragInstall(const Config* cfg)
{
	config = cfg;
	QueryPerformanceFrequency(&counterFrequency);
	ownerThread = GetCurrentThreadId();
	HINSTANCE instance = GetModuleHandleW(NULL);

	keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, instance, 0);
	mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, instance, 0);

	logWrite("hooks installed: keyboard=%p mouse=%p altdrag=%d mods=0x%X move=%d resize=%d (error %lu)",
		(void*)keyboardHook, (void*)mouseHook, (int)cfg->altdragEnabled, cfg->altdragMods,
		cfg->altdragMoveButton, cfg->altdragResizeButton, (unsigned long)GetLastError());

	return keyboardHook != NULL && mouseHook != NULL;
}

void altdragUninstall(void)
{
	if (action != ACTION_NONE) {
		action = ACTION_NONE;
		tilingSetSuspended(false);
	}

	if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
	if (mouseHook) UnhookWindowsHookEx(mouseHook);
	keyboardHook = NULL;
	mouseHook = NULL;
}

void altdragRefreshHooks(void)
{
	// Never swap hooks in the middle of a drag or while a modifier is held.
	if (!config || action != ACTION_NONE || modsDown != 0) {
		return;
	}

	HINSTANCE instance = GetModuleHandleW(NULL);
	HHOOK newKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, instance, 0);
	HHOOK newMouse = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, instance, 0);

	if (keyboardHook && newKeyboard) UnhookWindowsHookEx(keyboardHook);
	if (mouseHook && newMouse) UnhookWindowsHookEx(mouseHook);

	if (newKeyboard) keyboardHook = newKeyboard;
	if (newMouse) mouseHook = newMouse;
}
