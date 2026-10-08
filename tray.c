#include "tray.h"
#include "layout.h"
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_TRAYICON (WM_APP + 2)
#define PROMOTE_TIMER 1
#define PROMOTE_ATTEMPTS 6
#define PROMOTE_INTERVAL_MS 1500

static const wchar_t* const windowClass = L"LightWMTrayWindow";

static HWND trayWindow = NULL;
static HICON iconOn = NULL, iconOff = NULL;
static NOTIFYICONDATAW data;
static UINT taskbarCreated = 0;
static TrayHandler handler = NULL;
static bool lastEnabled = true;
static int lastDesktop = 0;
static int promoteAttempts = 0;
static bool added = false;

static void fillRect(uint32_t* pixels, int size, LRect r, uint32_t color)
{
	for (int y = r.top; y < r.bottom; y++) {
		for (int x = r.left; x < r.right; x++) {
			if (x >= 0 && y >= 0 && x < size && y < size) pixels[y * size + x] = color;
		}
	}
}

// Draws the master/stack layout LightWM produces: one tall tile and two stacked ones.
static HICON createIcon(int size, bool on)
{
	BITMAPINFOHEADER header;
	memset(&header, 0, sizeof header);
	header.biSize = sizeof header;
	header.biWidth = size;
	header.biHeight = -size;
	header.biPlanes = 1;
	header.biBitCount = 32;
	header.biCompression = BI_RGB;

	uint32_t* pixels = NULL;
	HBITMAP color = CreateDIBSection(NULL, (BITMAPINFO*)&header, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
	if (!color || !pixels) {
		return NULL;
	}

	memset(pixels, 0, (size_t)size * size * 4);

	int margin = size / 8;
	LRect area = { margin, margin, size - margin, size - margin };
	fillRect(pixels, size, area, 0xFF1E1E2E);

	LRect tiles[3];
	int gap = size / 10 < 1 ? 1 : size / 10;
	layoutCompute(LAYOUT_MASTER_STACK, area, 3, gap, 50, tiles);

	fillRect(pixels, size, tiles[0], on ? 0xFF89B4FA : 0xFF6C7086);
	fillRect(pixels, size, tiles[1], on ? 0xFFA6E3A1 : 0xFF585B70);
	fillRect(pixels, size, tiles[2], on ? 0xFFF9E2AF : 0xFF45475A);

	HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
	ICONINFO info;
	memset(&info, 0, sizeof info);
	info.fIcon = TRUE;
	info.hbmMask = mask;
	info.hbmColor = color;

	HICON icon = CreateIconIndirect(&info);
	DeleteObject(color);
	DeleteObject(mask);
	return icon;
}

static void fillTip(void)
{
	if (lastDesktop > 0) {
		swprintf(data.szTip, 128, L"LightWM - tiling %ls - desktop %d", lastEnabled ? L"on" : L"off", lastDesktop);
	} else {
		swprintf(data.szTip, 128, L"LightWM - tiling %ls", lastEnabled ? L"on" : L"off");
	}
}

static void addIcon(void)
{
	data.hIcon = lastEnabled ? iconOn : iconOff;
	fillTip();
	added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

// Windows 11 tucks new tray icons into the overflow menu. Its per-icon settings live in
// HKCU\Control Panel\NotifyIconSettings\<n>; IsPromoted=1 puts the icon in the main tray area.
static bool promoteIcon(void)
{
	HKEY root;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) {
		return true;   // Windows 10: no such setting, nothing to do
	}

	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	const wchar_t* exeName = wcsrchr(exe, L'\\');
	exeName = exeName ? exeName + 1 : exe;

	bool found = false;

	for (DWORD i = 0;; i++) {
		wchar_t name[64];
		DWORD nameLength = 64;
		if (RegEnumKeyExW(root, i, name, &nameLength, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

		HKEY entry;
		if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WRITE, &entry) != ERROR_SUCCESS) continue;

		wchar_t path[MAX_PATH];
		DWORD size = sizeof path, type = 0;
		if (RegQueryValueExW(entry, L"ExecutablePath", NULL, &type, (BYTE*)path, &size) == ERROR_SUCCESS && type == REG_SZ) {
			// Paths under known folders (like Downloads) are stored as {GUID}\..., so compare the file name.
			const wchar_t* entryName = wcsrchr(path, L'\\');
			entryName = entryName ? entryName + 1 : path;

			if (_wcsicmp(entryName, exeName) == 0) {
				DWORD promoted = 1;
				RegSetValueExW(entry, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&promoted, sizeof promoted);
				found = true;
			}
		}
		RegCloseKey(entry);
	}

	RegCloseKey(root);
	return found;
}

static void showMenu(void)
{
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING | (lastEnabled ? MF_CHECKED : 0), TRAY_TOGGLE_TILING, L"Tiling enabled");
	AppendMenuW(menu, MF_STRING, TRAY_RETILE, L"Retile now");
	AppendMenuW(menu, MF_STRING, TRAY_RELOAD, L"Reload config");
	AppendMenuW(menu, MF_STRING, TRAY_EDIT_CONFIG, L"Open config file");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, TRAY_QUIT, L"Quit LightWM");

	POINT cursor;
	GetCursorPos(&cursor);

	// Required so the menu closes when you click elsewhere.
	SetForegroundWindow(trayWindow);
	int command = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, trayWindow, NULL);
	PostMessageW(trayWindow, WM_NULL, 0, 0);
	DestroyMenu(menu);

	if (command && handler) {
		handler(command);
	}
}

static LRESULT CALLBACK trayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_TRAYICON) {
		if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
			showMenu();
		} else if (lparam == WM_LBUTTONDBLCLK && handler) {
			handler(TRAY_TOGGLE_TILING);
		}
		return 0;
	}

	if (message == taskbarCreated && taskbarCreated != 0) {
		addIcon();   // Explorer restarted: the icon has to be added again
		return 0;
	}

	if (message == WM_TIMER && wparam == PROMOTE_TIMER) {
		if (++promoteAttempts >= PROMOTE_ATTEMPTS || (added && promoteIcon())) {
			KillTimer(window, PROMOTE_TIMER);
		}
		return 0;
	}

	return DefWindowProcW(window, message, wparam, lparam);
}

bool trayInit(HINSTANCE instance, TrayHandler commandHandler)
{
	handler = commandHandler;

	WNDCLASSEXW windowClassInfo;
	memset(&windowClassInfo, 0, sizeof windowClassInfo);
	windowClassInfo.cbSize = sizeof windowClassInfo;
	windowClassInfo.lpfnWndProc = trayProc;
	windowClassInfo.hInstance = instance;
	windowClassInfo.lpszClassName = windowClass;
	if (!RegisterClassExW(&windowClassInfo)) {
		return false;
	}

	trayWindow = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass, L"LightWM", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
	if (!trayWindow) {
		return false;
	}

	int size = GetSystemMetrics(SM_CXSMICON);
	if (size < 16) size = 16;
	iconOn = createIcon(size, true);
	iconOff = createIcon(size, false);

	taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

	memset(&data, 0, sizeof data);
	data.cbSize = sizeof data;
	data.hWnd = trayWindow;
	data.uID = 1;
	data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	data.uCallbackMessage = WM_TRAYICON;

	addIcon();
	SetTimer(trayWindow, PROMOTE_TIMER, PROMOTE_INTERVAL_MS, NULL);
	return added;
}

void trayUpdate(bool tilingEnabled, int desktop)
{
	if (!trayWindow || !added) return;
	if (tilingEnabled == lastEnabled && desktop == lastDesktop) return;

	lastEnabled = tilingEnabled;
	lastDesktop = desktop;

	data.uFlags = NIF_ICON | NIF_TIP;
	data.hIcon = lastEnabled ? iconOn : iconOff;
	fillTip();
	Shell_NotifyIconW(NIM_MODIFY, &data);
	data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void trayCleanup(void)
{
	if (trayWindow) {
		Shell_NotifyIconW(NIM_DELETE, &data);
		DestroyWindow(trayWindow);
		trayWindow = NULL;
	}

	if (iconOn) DestroyIcon(iconOn);
	if (iconOff) DestroyIcon(iconOff);
	iconOn = iconOff = NULL;
	added = false;
}
