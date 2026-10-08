#include "log.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

static FILE* logFile = NULL;

void logInit(bool enabled)
{
	if (!enabled || logFile) {
		return;
	}

	wchar_t path[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	while (length > 0 && path[length - 1] != L'\\') length--;
	path[length] = 0;
	wcsncat(path, L"lightwm.log", MAX_PATH - wcslen(path) - 1);

	logFile = _wfopen(path, L"a");
	if (logFile) {
		fprintf(logFile, "---- LightWM started ----\n");
		fflush(logFile);
	}
}

void logWrite(const char* format, ...)
{
	if (!logFile) {
		return;
	}

	fprintf(logFile, "[%lu] ", (unsigned long)GetTickCount());

	va_list args;
	va_start(args, format);
	vfprintf(logFile, format, args);
	va_end(args);

	fputc('\n', logFile);
	fflush(logFile);
}

void logClose(void)
{
	if (logFile) {
		fclose(logFile);
		logFile = NULL;
	}
}
