#include <windows.h>
#include "error.h"
#include <stdio.h>

void reportGeneralError(const WCHAR* message)
{
	MessageBoxW(NULL, message, L"LightWM", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

void reportWin32Error(const WCHAR* message)
{
	DWORD lastError = GetLastError();
	WCHAR* messageBuffer = NULL;

	if (FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		lastError,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPWSTR)&messageBuffer,
		0,
		NULL) == 0) {
		reportGeneralError(message);
		return;
	}

	WCHAR combined[4096];
	swprintf(combined, 4096, L"%ls\n\n%ls", message, messageBuffer);
	reportGeneralError(combined);

	LocalFree(messageBuffer);
}
