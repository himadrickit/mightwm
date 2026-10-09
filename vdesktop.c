#include "vdesktop.h"
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Public IVirtualDesktopManager (documented in shobjidl_core.h)
// ---------------------------------------------------------------------------------------------

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

static const GUID clsidVdManager = { 0xAA509086, 0x5CA9, 0x4C25, { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };
static const GUID iidVdManager = { 0xA5CD92FF, 0x29BE, 0x454C, { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };

static IVdManager* publicManager = NULL;

// ---------------------------------------------------------------------------------------------
// Backend 1: VirtualDesktopAccessor.dll
// ---------------------------------------------------------------------------------------------

typedef int (__cdecl *FnVoid)(void);
typedef int (__cdecl *FnInt)(int);
typedef int (__cdecl *FnWindow)(HWND);
typedef int (__cdecl *FnWindowInt)(HWND, int);

static HMODULE accessor = NULL;
static FnVoid pGetCount, pGetCurrent, pCreate;
static FnInt pGoto;
static FnWindowInt pMove;
static FnWindow pWindowDesktop;

static bool loadAccessor(wchar_t* error, size_t errorCount)
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
		swprintf(error, errorCount, L"VirtualDesktopAccessor.dll was not found next to lightwm.exe.\n");
		return false;
	}

	pGetCount = (FnVoid)(void*)GetProcAddress(accessor, "GetDesktopCount");
	pGetCurrent = (FnVoid)(void*)GetProcAddress(accessor, "GetCurrentDesktopNumber");
	pGoto = (FnInt)(void*)GetProcAddress(accessor, "GoToDesktopNumber");
	pMove = (FnWindowInt)(void*)GetProcAddress(accessor, "MoveWindowToDesktopNumber");
	pWindowDesktop = (FnWindow)(void*)GetProcAddress(accessor, "GetWindowDesktopNumber");
	pCreate = (FnVoid)(void*)GetProcAddress(accessor, "CreateDesktop");

	if (!pGetCount || !pGetCurrent || !pGoto || !pMove) {
		swprintf(error, errorCount,
			L"VirtualDesktopAccessor.dll is missing required exports (GetDesktopCount, GetCurrentDesktopNumber, "
			L"GoToDesktopNumber, MoveWindowToDesktopNumber). Use version 2.x or newer.\n");
		FreeLibrary(accessor);
		accessor = NULL;
		pGetCount = pGetCurrent = pCreate = NULL;
		pGoto = NULL;
		pMove = NULL;
		pWindowDesktop = NULL;
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------------------------
// Backend 2: built-in client for the undocumented shell interfaces
//
// The interface IDs and method order below come from the community reverse engineering of
// explorer.exe (MScholtes/VirtualDesktop, Ciantic/VirtualDesktopAccessor). Microsoft changes
// them between Windows builds, so each candidate ID is offered to the shell and only an ID the
// shell accepts is used. Two layouts exist:
//   layout A (Windows 10):  methods take no monitor argument
//   layout B (Windows 11):  methods take a monitor/window argument (passed as NULL = "current")
// ---------------------------------------------------------------------------------------------

typedef struct {
	const GUID* managerIid;
	const GUID* desktopIid;
	bool monitorArg;
} Variant;

static const GUID clsidImmersiveShell = { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 } };
static const GUID iidServiceProvider = { 0x6D5140C1, 0x7436, 0x11CE, { 0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA } };
static const GUID clsidVdManagerInternal = { 0xC5E0CDCA, 0x7B6E, 0x41B2, { 0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B } };
static const GUID iidApplicationViewCollection = { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5 } };

static const GUID iidManagerWin10 = { 0xF31574D6, 0xB682, 0x4CDC, { 0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6 } };
static const GUID iidDesktopWin10 = { 0xFF72FFDD, 0xBE7E, 0x43FC, { 0x9C, 0x03, 0xAD, 0x81, 0x68, 0x1E, 0x88, 0xE4 } };
static const GUID iidManagerWin11Early = { 0xB2F925B9, 0x5A0F, 0x4D2E, { 0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10 } };
static const GUID iidDesktopWin11Early = { 0x536D3495, 0xB208, 0x4CC9, { 0xAE, 0x26, 0xDE, 0x81, 0x11, 0x27, 0x5B, 0xF8 } };
static const GUID iidManagerWin11Late = { 0xA3175F2D, 0x239C, 0x4BD2, { 0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E } };
static const GUID iidManagerWin11Newest = { 0x53F5CA0B, 0x158F, 0x4124, { 0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27 } };
static const GUID iidDesktopWin11Late = { 0x3F07F4BE, 0xB107, 0x441A, { 0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C } };

typedef HRESULT (STDMETHODCALLTYPE *FnQueryService)(void*, const GUID*, const GUID*, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnOut)(void*, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnMonOut)(void*, HWND, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnObj)(void*, void*);
typedef HRESULT (STDMETHODCALLTYPE *FnMonObj)(void*, HWND, void*);
typedef HRESULT (STDMETHODCALLTYPE *FnTwoObj)(void*, void*, void*);
typedef HRESULT (STDMETHODCALLTYPE *FnUIntOut)(void*, UINT*);
typedef HRESULT (STDMETHODCALLTYPE *FnMonUIntOut)(void*, HWND, UINT*);
typedef HRESULT (STDMETHODCALLTYPE *FnGetAt)(void*, UINT, const GUID*, void**);
typedef HRESULT (STDMETHODCALLTYPE *FnGuidOut)(void*, GUID*);
typedef HRESULT (STDMETHODCALLTYPE *FnGetView)(void*, HWND, void**);
typedef ULONG (STDMETHODCALLTYPE *FnRelease)(void*);

#define VTABLE(object) (*(void***)(object))

// Slots of IVirtualDesktopManagerInternal (counting IUnknown's three methods as 0..2)
enum { SLOT_COUNT = 3, SLOT_MOVE_VIEW = 4, SLOT_CURRENT = 6 };
// IObjectArray: GetCount = 3, GetAt = 4. IVirtualDesktop: GetId = 4.

static void* manager = NULL;
static void* viewCollection = NULL;
static const Variant* variant = NULL;
// IApplicationViewCollection derives from IUnknown: GetViews(3) GetViewsByZOrder(4)
// GetViewsByAppUserModelId(5) GetViewForHwnd(6)
static const int viewSlot = 6;
static bool builtinReady = false;

static void releaseObject(void* object)
{
	if (object) {
		((FnRelease)VTABLE(object)[2])(object);
	}
}

static DWORD windowsBuild(void)
{
	typedef struct {
		DWORD size, major, minor, build, platform;
		WCHAR csd[128];
	} VersionInfo;
	typedef LONG (WINAPI *FnRtlGetVersion)(VersionInfo*);

	FnRtlGetVersion rtlGetVersion = (FnRtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	VersionInfo info;

	memset(&info, 0, sizeof info);
	info.size = sizeof info;
	if (!rtlGetVersion || rtlGetVersion(&info) != 0) {
		return 0;
	}
	return info.build;
}

static bool comCount(UINT* count)
{
	HRESULT hr = variant->monitorArg
		? ((FnMonUIntOut)VTABLE(manager)[SLOT_COUNT])(manager, NULL, count)
		: ((FnUIntOut)VTABLE(manager)[SLOT_COUNT])(manager, count);
	return SUCCEEDED(hr);
}

static bool comDesktops(void** array)
{
	HRESULT hr = variant->monitorArg
		? ((FnMonOut)VTABLE(manager)[8])(manager, NULL, array)
		: ((FnOut)VTABLE(manager)[7])(manager, array);
	return SUCCEEDED(hr) && *array;
}

// Returns desktop number `index` (0-based); the caller releases it.
static void* comDesktopAt(UINT index)
{
	void* array = NULL;
	void* desktop = NULL;

	if (!comDesktops(&array)) {
		return NULL;
	}

	HRESULT hr = ((FnGetAt)VTABLE(array)[4])(array, index, variant->desktopIid, &desktop);
	releaseObject(array);
	return SUCCEEDED(hr) ? desktop : NULL;
}

static bool comDesktopId(void* desktop, GUID* id)
{
	return SUCCEEDED(((FnGuidOut)VTABLE(desktop)[4])(desktop, id));
}

static int comIndexOfId(const GUID* wanted)
{
	UINT count = 0;
	if (!comCount(&count)) return 0;

	for (UINT i = 0; i < count; i++) {
		void* desktop = comDesktopAt(i);
		GUID id;
		bool match = desktop && comDesktopId(desktop, &id) && memcmp(&id, wanted, sizeof id) == 0;
		releaseObject(desktop);

		if (match) return (int)i + 1;
	}
	return 0;
}

static int comCurrent(void)
{
	void* current = NULL;
	HRESULT hr = variant->monitorArg
		? ((FnMonOut)VTABLE(manager)[SLOT_CURRENT])(manager, NULL, &current)
		: ((FnOut)VTABLE(manager)[SLOT_CURRENT])(manager, &current);

	GUID id;
	int number = 0;
	if (SUCCEEDED(hr) && current && comDesktopId(current, &id)) {
		number = comIndexOfId(&id);
	}
	releaseObject(current);
	return number;
}

static bool comCreate(void)
{
	void* created = NULL;
	HRESULT hr = variant->monitorArg
		? ((FnMonOut)VTABLE(manager)[11])(manager, NULL, &created)
		: ((FnOut)VTABLE(manager)[10])(manager, &created);

	releaseObject(created);
	return SUCCEEDED(hr);
}

static bool comSwitch(int number)
{
	void* desktop = comDesktopAt((UINT)number - 1);
	if (!desktop) return false;

	HRESULT hr = variant->monitorArg
		? ((FnMonObj)VTABLE(manager)[10])(manager, NULL, desktop)
		: ((FnObj)VTABLE(manager)[9])(manager, desktop);

	releaseObject(desktop);
	return SUCCEEDED(hr);
}

static bool comMove(HWND window, int number)
{
	if (!viewCollection) return false;

	void* desktop = comDesktopAt((UINT)number - 1);
	if (!desktop) return false;

	void* view = NULL;
	HRESULT hr = ((FnGetView)VTABLE(viewCollection)[viewSlot])(viewCollection, window, &view);
	bool ok = false;

	if (SUCCEEDED(hr) && view) {
		ok = SUCCEEDED(((FnTwoObj)VTABLE(manager)[SLOT_MOVE_VIEW])(manager, view, desktop));
	}

	releaseObject(view);
	releaseObject(desktop);
	return ok;
}

static bool initBuiltin(wchar_t* error, size_t errorCount)
{
	static const Variant win10 = { &iidManagerWin10, &iidDesktopWin10, false };
	static const Variant win11Early = { &iidManagerWin11Early, &iidDesktopWin11Early, true };
	static const Variant win11Newest = { &iidManagerWin11Newest, &iidDesktopWin11Late, true };
	static const Variant win11Late = { &iidManagerWin11Late, &iidDesktopWin11Late, true };

	DWORD build = windowsBuild();
	const Variant* candidates[3] = { NULL, NULL, NULL };

	if (build >= 22621) {
		candidates[0] = &win11Newest;
		candidates[1] = &win11Late;
	} else if (build >= 22000) {
		candidates[0] = &win11Early;
	} else if (build >= 17763) {
		candidates[0] = &win10;
	} else {
		swprintf(error, errorCount, L"Windows build %lu is too old for the built-in virtual desktop support (needs 17763+).\n", build);
		return false;
	}

	void* provider = NULL;
	HRESULT hr = CoCreateInstance(&clsidImmersiveShell, NULL, CLSCTX_LOCAL_SERVER, &iidServiceProvider, &provider);
	if (FAILED(hr) || !provider) {
		hr = CoCreateInstance(&clsidImmersiveShell, NULL, CLSCTX_ALL, &iidServiceProvider, &provider);
	}
	if (FAILED(hr) || !provider) {
		swprintf(error, errorCount, L"Could not reach the Windows shell for virtual desktops (Windows build %lu, error 0x%08lX).\n",
			build, (unsigned long)hr);
		return false;
	}

	FnQueryService queryService = (FnQueryService)VTABLE(provider)[3];

	HRESULT lastError = E_FAIL;
	for (int i = 0; i < 3 && candidates[i] && !manager; i++) {
		void* found = NULL;
		lastError = queryService(provider, &clsidVdManagerInternal, candidates[i]->managerIid, &found);
		if (SUCCEEDED(lastError) && found) {
			manager = found;
			variant = candidates[i];
		}
	}

	// Only needed to move windows to another desktop.
	queryService(provider, &iidApplicationViewCollection, &iidApplicationViewCollection, &viewCollection);
	releaseObject(provider);

	if (!manager) {
		swprintf(error, errorCount,
			L"This Windows build (%lu) uses virtual desktop interfaces LightWM does not know yet (error 0x%08lX).\n"
			L"Put VirtualDesktopAccessor.dll next to lightwm.exe, or report the build number.\n", build, (unsigned long)lastError);
		return false;
	}

	// Sanity check: the shell must answer sensibly before anything depends on it.
	UINT count = 0;
	void* first = NULL;
	bool sane = comCount(&count) && count >= 1 && count <= 200 && (first = comDesktopAt(0)) != NULL;
	releaseObject(first);

	if (!sane) {
		releaseObject(manager);
		releaseObject(viewCollection);
		manager = viewCollection = NULL;
		swprintf(error, errorCount, L"The shell's virtual desktop interface (Windows build %lu) did not answer as expected.\n", build);
		return false;
	}

	builtinReady = true;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

bool vdInit(int backendPreference, wchar_t* error, size_t errorCount)
{
	error[0] = 0;

	CoCreateInstance(&clsidVdManager, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, &iidVdManager, (void**)&publicManager);

	wchar_t dllProblem[512] = L"";
	wchar_t builtinProblem[512] = L"";

	if (backendPreference != DESKTOP_BACKEND_BUILTIN && loadAccessor(dllProblem, 512)) {
		return true;
	}

	if (backendPreference != DESKTOP_BACKEND_DLL && initBuiltin(builtinProblem, 512)) {
		return true;
	}

	swprintf(error, errorCount,
		L"Workspace switching (alt+1..9) is disabled.\n%ls%ls"
		L"Tiling and everything else works normally.\n", dllProblem, builtinProblem);
	return false;
}

void vdCleanup(void)
{
	if (publicManager) {
		publicManager->lpVtbl->Release(publicManager);
		publicManager = NULL;
	}

	releaseObject(viewCollection);
	releaseObject(manager);
	viewCollection = manager = NULL;
	builtinReady = false;

	if (accessor) {
		FreeLibrary(accessor);
		accessor = NULL;
	}
}

bool vdCanControl(void)
{
	return accessor != NULL || builtinReady;
}

int vdCount(void)
{
	if (accessor) return pGetCount ? pGetCount() : 0;

	UINT count = 0;
	return builtinReady && comCount(&count) ? (int)count : 0;
}

int vdCurrent(void)
{
	if (accessor) {
		int zeroBased = pGetCurrent();
		return zeroBased >= 0 ? zeroBased + 1 : 0;
	}

	return builtinReady ? comCurrent() : 0;
}

static bool ensureDesktopExists(int number)
{
	int count = vdCount();

	while (count > 0 && count < number) {
		bool created = accessor ? (pCreate && pCreate() >= 0) : comCreate();
		if (!created) break;

		int updated = vdCount();
		if (updated <= count) break;
		count = updated;
	}

	return number <= count;
}

bool vdGoto(int number)
{
	if (!vdCanControl() || number < 1 || !ensureDesktopExists(number)) {
		return false;
	}

	return accessor ? pGoto(number - 1) >= 0 : comSwitch(number);
}

bool vdMoveWindow(HWND window, int number)
{
	if (!vdCanControl() || number < 1 || !ensureDesktopExists(number)) {
		return false;
	}

	return accessor ? pMove(window, number - 1) >= 0 : comMove(window, number);
}

int vdWindowDesktop(HWND window)
{
	if (accessor) {
		if (!pWindowDesktop) return 0;
		int zeroBased = pWindowDesktop(window);
		return zeroBased >= 0 ? zeroBased + 1 : 0;
	}

	GUID id;
	if (!builtinReady || !publicManager || FAILED(publicManager->lpVtbl->GetWindowDesktopId(publicManager, window, &id))) {
		return 0;
	}
	return comIndexOfId(&id);
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
