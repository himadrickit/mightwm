#include "vdesktop.h"
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

typedef struct IVdManager IVdManager;

typedef struct {
	HRESULT (STDMETHODCALLTYPE *QueryInterface)(IVdManager*, REFIID, void**);
	ULONG (STDMETHODCALLTYPE *AddRef)(IVdManager*);
	ULONG (STDMETHODCALLTYPE *Release)(IVdManager*);
	HRESULT (STDMETHODCALLTYPE *IsWindowOnCurrentVirtualDesktop)(IVdManager*, HWND, BOOL*);
	HRESULT (STDMETHODCALLTYPE *GetWindowDesktopId)(IVdManager*, HWND, GUID*);
	HRESULT (STDMETHODCALLTYPE *MoveWindowToDesktop)(IVdManager*, HWND, REFGUID);
} IVdManagerVtbl;

struct IVdManager {
	const IVdManagerVtbl* lpVtbl;
};

// IVirtualDesktopManager, documented in shobjidl_core.h
static const GUID clsidVdManager = { 0xAA509086, 0x5CA9, 0x4C25, { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };
static const GUID iidVdManager = { 0xA5CD92FF, 0x29BE, 0x454C, { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };

typedef int (__cdecl *FnVoid)(void);
typedef int (__cdecl *FnInt)(int);
typedef int (__cdecl *FnWindow)(HWND);
typedef int (__cdecl *FnWindowInt)(HWND, int);

static IVdManager* publicManager = NULL;
static HMODULE accessor = NULL;
static FnVoid pGetCount, pGetCurrent, pCreate;
static FnInt pGoto;
static FnWindowInt pMove;
static FnWindow pWindowDesktop;

static void loadAccessor(wchar_t* error, size_t errorCount)
{
	wchar_t path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);

	while (length > 0 && path[length - 1] != L'\\') {
		length--;
	}
	path[length] = 0;
	wcsncat(path, L"VirtualDesktopAccessor.dll", MAX_PATH - wcslen(path) - 1);

	accessor = LoadLibraryW(path);
	if (!accessor) {
		swprintf(error, errorCount,
			L"VirtualDesktopAccessor.dll was not found next to lightwm.exe.\n"
			L"Workspace switching (alt+1..9) is disabled until it is added.\n"
			L"Tiling and everything else works normally.");
		return;
	}

	pGetCount = (FnVoid)GetProcAddress(accessor, "GetDesktopCount");
	pGetCurrent = (FnVoid)GetProcAddress(accessor, "GetCurrentDesktopNumber");
	pGoto = (FnInt)GetProcAddress(accessor, "GoToDesktopNumber");
	pMove = (FnWindowInt)GetProcAddress(accessor, "MoveWindowToDesktopNumber");
	pWindowDesktop = (FnWindow)GetProcAddress(accessor, "GetWindowDesktopNumber");
	pCreate = (FnVoid)GetProcAddress(accessor, "CreateDesktop");

	if (!pGetCount || !pGetCurrent || !pGoto || !pMove) {
		swprintf(error, errorCount,
			L"VirtualDesktopAccessor.dll is missing required exports (GetDesktopCount, GetCurrentDesktopNumber, "
			L"GoToDesktopNumber, MoveWindowToDesktopNumber). Use version 2.x or newer.");
		FreeLibrary(accessor);
		accessor = NULL;
		pGetCount = pGetCurrent = pCreate = NULL;
		pGoto = NULL;
		pMove = NULL;
		pWindowDesktop = NULL;
	}
}

bool vdInit(wchar_t* error, size_t errorCount)
{
	error[0] = 0;

	CoCreateInstance(&clsidVdManager, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &iidVdManager, (void**)&publicManager);

	loadAccessor(error, errorCount);
	return accessor != NULL;
}

void vdCleanup(void)
{
	if (publicManager) {
		publicManager->lpVtbl->Release(publicManager);
		publicManager = NULL;
	}

	if (accessor) {
		FreeLibrary(accessor);
		accessor = NULL;
	}
}

bool vdCanControl(void)
{
	return accessor != NULL;
}

int vdCount(void)
{
	return pGetCount ? pGetCount() : 0;
}

int vdCurrent(void)
{
	if (!pGetCurrent) {
		return 0;
	}

	int zeroBased = pGetCurrent();
	return zeroBased >= 0 ? zeroBased + 1 : 0;
}

static bool ensureDesktopExists(int number)
{
	int count = vdCount();

	while (count < number && pCreate) {
		if (pCreate() < 0) {
			break;
		}

		int updated = vdCount();
		if (updated <= count) {
			break;
		}
		count = updated;
	}

	return number <= count;
}

bool vdGoto(int number)
{
	if (!pGoto || number < 1 || !ensureDesktopExists(number)) {
		return false;
	}

	return pGoto(number - 1) >= 0;
}

bool vdMoveWindow(HWND window, int number)
{
	if (!pMove || number < 1 || !ensureDesktopExists(number)) {
		return false;
	}

	return pMove(window, number - 1) >= 0;
}

int vdWindowDesktop(HWND window)
{
	if (!pWindowDesktop) {
		return 0;
	}

	int zeroBased = pWindowDesktop(window);
	return zeroBased >= 0 ? zeroBased + 1 : 0;
}

bool vdWindowOnCurrent(HWND window)
{
	if (!publicManager) {
		return true;
	}

	BOOL onCurrent = TRUE;
	if (FAILED(publicManager->lpVtbl->IsWindowOnCurrentVirtualDesktop(publicManager, window, &onCurrent))) {
		return true;
	}

	return onCurrent != FALSE;
}
