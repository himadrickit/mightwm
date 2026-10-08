# MSVC (run from a "x64 Native Tools Command Prompt"):  nmake        -> release\lightwm.exe
#                                                        nmake debug  -> debug\lightwm.exe
# MinGW-w64:  mingw32-make -f Makefile.mingw
SRCS = wm.c tiling.c layout.c kdl.c config.c keyboard.c vdesktop.c altdrag.c tray.c error.c
LIBS = kernel32.lib user32.lib gdi32.lib advapi32.lib dwmapi.lib ole32.lib shell32.lib
CFLAGS = /nologo /utf-8 /W3 /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS

all: release

debug: prep
	cl $(CFLAGS) /DDEBUG /Zi $(SRCS) /Fe:debug\lightwm.exe /Fo:debug\ /link $(LIBS) /subsystem:windows

release: prep
	cl $(CFLAGS) /Ox $(SRCS) /Fe:release\lightwm.exe /Fo:release\ /link $(LIBS) /subsystem:windows

prep:
	cmd /c IF NOT EXIST debug mkdir debug
	cmd /c IF NOT EXIST release mkdir release

clean:
	cmd /c IF EXIST debug rd /S /Q debug
	cmd /c IF EXIST release rd /S /Q release
