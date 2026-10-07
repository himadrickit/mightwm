#include "keyboard.h"
#include <stdio.h>
#include <wchar.h>

#define DUPLICATE_WINDOW_MS 60

static const Config* current = NULL;
static int registeredCount = 0;
static WPARAM lastId = 0;
static DWORD lastTick = 0;

void keyboardInit(const Config* cfg)
{
	current = cfg;
}

int keyboardMatch(UINT mods, UINT vk)
{
	if (!current) return 0;

	for (int i = 0; i < current->bindCount; i++) {
		if (current->binds[i].vk == vk && current->binds[i].mods == mods) {
			return i + 1;
		}
	}
	return 0;
}

const Binding* keyboardLookup(WPARAM bindingId)
{
	if (!current || bindingId < 1 || (int)bindingId > current->bindCount) {
		return NULL;
	}

	return &current->binds[bindingId - 1];
}

static void describeCombo(const Binding* b, wchar_t* out, size_t count)
{
	swprintf(out, count, L"%ls%ls%ls%ls vk=0x%02X",
		(b->mods & MOD_CONTROL) ? L"ctrl+" : L"",
		(b->mods & MOD_ALT) ? L"alt+" : L"",
		(b->mods & MOD_SHIFT) ? L"shift+" : L"",
		(b->mods & MOD_WIN) ? L"win+" : L"",
		b->vk);
}

bool keyboardRegister(wchar_t* message, size_t messageCount)
{
	bool allOk = true;
	size_t used = 0;

	message[0] = 0;
	registeredCount = 0;
	if (!current) return true;

	for (int i = 0; i < current->bindCount; i++) {
		const Binding* b = &current->binds[i];
		registeredCount = i + 1;

		if (RegisterHotKey(NULL, i + 1, b->mods | MOD_NOREPEAT, b->vk)) {
			continue;
		}

		allOk = false;

		wchar_t combo[64];
		describeCombo(b, combo, 64);
		int written = swprintf(message + used, messageCount - used,
			L"%ls is also owned by another program. LightWM still uses it first through its keyboard hook "
			L"(not while an administrator window has focus).\n", combo);
		if (written > 0 && (size_t)written < messageCount - used) used += written;
	}

	return allOk;
}

void keyboardUnregister(void)
{
	for (int i = 0; i < registeredCount; i++) {
		UnregisterHotKey(NULL, i + 1);
	}
	registeredCount = 0;
}

bool keyboardIsDuplicate(WPARAM bindingId)
{
	DWORD now = GetTickCount();
	bool duplicate = bindingId == lastId && now - lastTick < DUPLICATE_WINDOW_MS;

	lastId = bindingId;
	lastTick = now;
	return duplicate;
}
