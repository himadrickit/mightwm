#include "config.h"
#include "kdl.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char* const defaultConfig =
"// LightWM configuration (KDL). Edit, then press $mod+shift+r to reload.\n"
"\n"
"vars {\n"
"    mod \"alt\"            // use \"win\" or \"ctrl+alt\" if you prefer\n"
"    term \"wt.exe\"\n"
"}\n"
"\n"
"general {\n"
"    gap 6                  // pixels between windows and around the screen edge\n"
"    autowidth 65 60 70     // main (left) window width in %: starts at the first, $mod+w cycles through them\n"
"    layout \"master-stack\"  // \"master-stack\", \"grid\" or \"columns\"\n"
"    float-size 70          // size (% of the screen) a window gets when you float it with $mod+t\n"
"    warp-cursor false      // move the mouse to the window you focus by keyboard\n"
"    auto-focus \"lost\"     // after a close or desktop switch focus the master (left) window:\n"
"                           // \"lost\" only if nothing is focused, \"always\", or \"off\"\n"
"    debug-log false        // true: write lightwm.log next to the exe (for bug reports)\n"
"\n"
"    // Windows never tiled, matched against window class or exe name.\n"
"    // float \"Calculator\" \"mpv.exe\"\n"
"}\n"
"\n"
"workspaces {\n"
"    follow-focus true      // focusing a window on another desktop switches to it\n"
"    block-windows-shortcuts true  // turn off Windows' own ctrl+win+d / ctrl+win+left/right\n"
"    backend \"auto\"        // \"auto\" (DLL if present, else built-in), \"dll\" or \"builtin\"\n"
"    movetype \"stay\"       // moving a window to another workspace: \"follow\" it there, or \"stay\" here\n"
"    goto \"$mod+{1-9}\"\n"
"    send \"$mod+shift+{1-9}\"\n"
"}\n"
"\n"
"altdrag {\n"
"    enabled true\n"
"    mod \"$mod\"\n"
"    move \"left\"           // mod + left drag moves, drop on another tile to swap\n"
"    resize \"right\"        // mod + right drag resizes (the 3x3 region you grab picks the edge)\n"
"}\n"
"\n"
"binds {\n"
"    $mod+h { focus-left }\n"
"    $mod+j { focus-down }\n"
"    $mod+k { focus-up }\n"
"    $mod+l { focus-right }\n"
"    $mod+period { focus-next }\n"
"    $mod+comma  { focus-prev }\n"
"\n"
"    $mod+shift+h { move-left }\n"
"    $mod+shift+j { move-down }\n"
"    $mod+shift+k { move-up }\n"
"    $mod+shift+l { move-right }\n"
"\n"
"    $mod+g { focus-master }\n"
"    $mod+f { fullscreen }\n"
"    $mod+m { monocle }\n"
"    $mod+t { toggle-floating }\n"
"    $mod+shift+t { retile }\n"
"    $mod+backslash { toggle-tiling }\n"
"    $mod+shift+q { close-window }\n"
"    $mod+shift+r { reload-config }\n"
"    $mod+shift+e { quit }\n"
"\n"
"    $mod+w { master-cycle }\n"
"    $mod+equal { master-grow }\n"
"    $mod+minus { master-shrink }\n"
"\n"
"    $mod+bracketleft  { workspace-prev }\n"
"    $mod+bracketright { workspace-next }\n"
"\n"
"    $mod+Return { spawn \"$term\" }\n"
"}\n";

typedef struct {
	char name[32];
	char* value;
} Var;

typedef struct {
	Config* cfg;
	Var vars[32];
	int varCount;
	char log[2048];
} Ctx;

static wchar_t configFilePath[MAX_PATH];

static void note(Ctx* ctx, const char* format, ...)
{
	size_t used = strlen(ctx->log);
	if (used + 2 >= sizeof ctx->log) {
		return;
	}

	va_list args;
	va_start(args, format);
	vsnprintf(ctx->log + used, sizeof ctx->log - used - 1, format, args);
	va_end(args);
	strcat(ctx->log, "\n");
}

static char* copyText(const char* s)
{
	size_t length = strlen(s);
	char* result = malloc(length + 1);
	memcpy(result, s, length + 1);
	return result;
}

static wchar_t* toWide(const char* s)
{
	int count = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t* result = malloc(sizeof(wchar_t) * (count + 1));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, result, count);
	result[count] = 0;
	return result;
}

static bool truthy(const char* s)
{
	return s && (!_stricmp(s, "true") || !_stricmp(s, "yes") || !_stricmp(s, "on") || !strcmp(s, "1"));
}

static void setVar(Ctx* ctx, const char* name, const char* value)
{
	for (int i = 0; i < ctx->varCount; i++) {
		if (strcmp(ctx->vars[i].name, name) == 0) {
			free(ctx->vars[i].value);
			ctx->vars[i].value = copyText(value);
			return;
		}
	}

	if (ctx->varCount >= 32 || strlen(name) >= sizeof ctx->vars[0].name) {
		note(ctx, "vars: too many or too long variable: %s", name);
		return;
	}

	strcpy(ctx->vars[ctx->varCount].name, name);
	ctx->vars[ctx->varCount].value = copyText(value);
	ctx->varCount++;
}

// Replaces $name with the value of the variable `name`. Caller frees the result.
static char* expand(Ctx* ctx, const char* s)
{
	size_t capacity = strlen(s) + 64, length = 0;
	char* out = malloc(capacity);

	while (*s) {
		const char* piece = s;
		size_t pieceLength = 1;

		if (*s == '$' && (isalnum((unsigned char)s[1]) || s[1] == '_')) {
			size_t nameLength = 0;
			while (isalnum((unsigned char)s[1 + nameLength]) || s[1 + nameLength] == '_') nameLength++;

			int found = -1;
			for (int i = 0; i < ctx->varCount; i++) {
				if (strlen(ctx->vars[i].name) == nameLength && strncmp(ctx->vars[i].name, s + 1, nameLength) == 0) {
					found = i;
					break;
				}
			}

			if (found >= 0) {
				piece = ctx->vars[found].value;
				pieceLength = strlen(piece);
				s += 1 + nameLength;
			} else {
				note(ctx, "unknown variable $%.*s", (int)nameLength, s + 1);
				pieceLength = 1 + nameLength;
				s += pieceLength;
			}
		} else {
			s++;
		}

		if (length + pieceLength + 1 > capacity) {
			capacity = (length + pieceLength + 1) * 2;
			out = realloc(out, capacity);
		}
		memcpy(out + length, piece, pieceLength);
		length += pieceLength;
	}

	out[length] = 0;
	return out;
}

static UINT modifierFromName(const char* name)
{
	if (!strcmp(name, "alt")) return MOD_ALT;
	if (!strcmp(name, "ctrl") || !strcmp(name, "control")) return MOD_CONTROL;
	if (!strcmp(name, "shift")) return MOD_SHIFT;
	if (!strcmp(name, "win") || !strcmp(name, "super") || !strcmp(name, "meta")) return MOD_WIN;
	return 0;
}

static const struct { const char* name; UINT vk; } keyNames[] = {
	{ "return", VK_RETURN }, { "enter", VK_RETURN }, { "space", VK_SPACE }, { "tab", VK_TAB },
	{ "escape", VK_ESCAPE }, { "esc", VK_ESCAPE }, { "backspace", VK_BACK }, { "delete", VK_DELETE },
	{ "del", VK_DELETE }, { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END },
	{ "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "left", VK_LEFT }, { "right", VK_RIGHT },
	{ "up", VK_UP }, { "down", VK_DOWN }, { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD },
	{ "slash", VK_OEM_2 }, { "backslash", VK_OEM_5 }, { "semicolon", VK_OEM_1 }, { "apostrophe", VK_OEM_7 },
	{ "quote", VK_OEM_7 }, { "minus", VK_OEM_MINUS }, { "equal", VK_OEM_PLUS }, { "plus", VK_OEM_PLUS },
	{ "bracketleft", VK_OEM_4 }, { "bracketright", VK_OEM_6 }, { "grave", VK_OEM_3 }, { "backtick", VK_OEM_3 },
	{ "print", VK_SNAPSHOT }, { "printscreen", VK_SNAPSHOT }, { "volumeup", VK_VOLUME_UP },
	{ "volumedown", VK_VOLUME_DOWN }, { "mute", VK_VOLUME_MUTE }, { "playpause", VK_MEDIA_PLAY_PAUSE },
	{ "nexttrack", VK_MEDIA_NEXT_TRACK }, { "prevtrack", VK_MEDIA_PREV_TRACK }
};

static bool keyFromName(const char* name, UINT* vk)
{
	size_t length = strlen(name);

	if (length == 1 && isalpha((unsigned char)name[0])) {
		*vk = (UINT)toupper((unsigned char)name[0]);
		return true;
	}
	if (length == 1 && isdigit((unsigned char)name[0])) {
		*vk = (UINT)name[0];
		return true;
	}

	if (name[0] == 'f' && length >= 2 && length <= 3 && isdigit((unsigned char)name[1])) {
		int number = atoi(name + 1);
		if (number >= 1 && number <= 24) {
			*vk = VK_F1 + number - 1;
			return true;
		}
	}

	if (!strncmp(name, "numpad", 6) && length == 7 && isdigit((unsigned char)name[6])) {
		*vk = VK_NUMPAD0 + (name[6] - '0');
		return true;
	}

	for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++) {
		if (!strcmp(name, keyNames[i].name)) {
			*vk = keyNames[i].vk;
			return true;
		}
	}

	return false;
}

static bool parseCombo(const char* text, UINT* mods, UINT* vk)
{
	*mods = 0;
	*vk = 0;

	const char* p = text;
	for (;;) {
		size_t length = 0;
		char token[32];

		while (p[length] && p[length] != '+') length++;
		if (length == 0 || length >= sizeof token) return false;

		for (size_t i = 0; i < length; i++) token[i] = (char)tolower((unsigned char)p[i]);
		token[length] = 0;

		UINT modifier = modifierFromName(token);
		if (modifier) {
			*mods |= modifier;
		} else {
			if (*vk || !keyFromName(token, vk)) return false;
		}

		p += length;
		if (*p == 0) break;
		p++;
	}

	return *vk != 0;
}

static const struct { const char* name; Action action; } actionNames[] = {
	{ "focus-next", ACT_FOCUS_NEXT }, { "focus-prev", ACT_FOCUS_PREV }, { "focus-previous", ACT_FOCUS_PREV },
	{ "focus-left", ACT_FOCUS_LEFT }, { "focus-right", ACT_FOCUS_RIGHT }, { "focus-up", ACT_FOCUS_UP }, { "focus-down", ACT_FOCUS_DOWN },
	{ "move-left", ACT_MOVE_LEFT }, { "move-right", ACT_MOVE_RIGHT }, { "move-up", ACT_MOVE_UP }, { "move-down", ACT_MOVE_DOWN },
	{ "move-win-left", ACT_MOVE_LEFT }, { "move-win-right", ACT_MOVE_RIGHT }, { "move-win-up", ACT_MOVE_UP }, { "move-win-down", ACT_MOVE_DOWN },
	{ "focus-master", ACT_FOCUS_MASTER }, { "close-window", ACT_CLOSE }, { "close", ACT_CLOSE },
	{ "toggle-floating", ACT_TOGGLE_FLOAT }, { "monocle", ACT_MONOCLE }, { "toggle-monocle", ACT_MONOCLE },
	{ "fullscreen", ACT_FULLSCREEN }, { "retile", ACT_RETILE }, { "toggle-tiling", ACT_TOGGLE_TILING },
	{ "reload-config", ACT_RELOAD }, { "quit", ACT_QUIT }, { "spawn", ACT_SPAWN },
	{ "goto", ACT_GOTO }, { "send", ACT_SEND }, { "send-follow", ACT_SEND_FOLLOW }, { "send-stay", ACT_SEND_STAY },
	{ "master-cycle", ACT_MASTER_CYCLE }, { "master-grow", ACT_MASTER_GROW }, { "master-shrink", ACT_MASTER_SHRINK },
	{ "workspace-next", ACT_WORKSPACE_NEXT }, { "workspace-prev", ACT_WORKSPACE_PREV },
	{ "switch-next-workspace", ACT_WORKSPACE_NEXT }, { "switch-previous-workspace", ACT_WORKSPACE_PREV }
};

static Action actionFromName(const char* name)
{
	char normalized[48];
	size_t i = 0;

	for (; name[i] && i < sizeof normalized - 1; i++) {
		char c = (char)tolower((unsigned char)name[i]);
		normalized[i] = (c == '_') ? '-' : c;
	}
	normalized[i] = 0;

	for (size_t k = 0; k < sizeof actionNames / sizeof actionNames[0]; k++) {
		if (!strcmp(normalized, actionNames[k].name)) {
			return actionNames[k].action;
		}
	}
	return ACT_NONE;
}

static void addBinding(Ctx* ctx, UINT mods, UINT vk, Action action, int arg, const char* text)
{
	Config* cfg = ctx->cfg;

	for (int i = 0; i < cfg->bindCount; i++) {
		if (cfg->binds[i].mods == mods && cfg->binds[i].vk == vk) {
			note(ctx, "duplicate keybind (the first one wins)");
			return;
		}
	}

	if (cfg->bindCount == cfg->bindCapacity) {
		cfg->bindCapacity = cfg->bindCapacity ? cfg->bindCapacity * 2 : 64;
		cfg->binds = realloc(cfg->binds, sizeof(Binding) * cfg->bindCapacity);
	}

	Binding* b = &cfg->binds[cfg->bindCount++];
	b->mods = mods;
	b->vk = vk;
	b->action = action;
	b->arg = arg;
	b->text = text ? toWide(text) : NULL;
}

static void parseVars(Ctx* ctx, const KdlNode* node)
{
	for (int i = 0; i < node->kidc; i++) {
		const KdlNode* var = node->kids[i];
		if (var->argc < 1) {
			note(ctx, "vars: %s has no value", var->name);
			continue;
		}
		setVar(ctx, var->name, var->args[0]);
	}
}

static void parseGeneral(Ctx* ctx, const KdlNode* node)
{
	Config* cfg = ctx->cfg;

	for (int i = 0; i < node->kidc; i++) {
		const KdlNode* n = node->kids[i];
		const char* value = n->argc ? n->args[0] : NULL;

		if (!strcmp(n->name, "gap") && value) {
			cfg->gap = atoi(value) < 0 ? 0 : atoi(value);
		} else if ((!strcmp(n->name, "master-width") || !strcmp(n->name, "autowidth") || !strcmp(n->name, "auto-width")) && value) {
			// One or more widths: the first is the starting width, `master-cycle` steps through the rest.
			cfg->widthPresetCount = 0;
			for (int a = 0; a < n->argc && cfg->widthPresetCount < MAX_WIDTH_PRESETS; a++) {
				int percent = atoi(n->args[a]);
				cfg->widthPresets[cfg->widthPresetCount++] = percent < 10 ? 10 : percent > 90 ? 90 : percent;
			}
			cfg->masterPercent = cfg->widthPresets[0];
		} else if (!strcmp(n->name, "layout") && value) {
			if (!strcmp(value, "master-stack")) cfg->layout = LAYOUT_MASTER_STACK;
			else if (!strcmp(value, "grid")) cfg->layout = LAYOUT_GRID;
			else if (!strcmp(value, "columns")) cfg->layout = LAYOUT_COLUMNS;
			else note(ctx, "general: unknown layout \"%s\"", value);
		} else if (!strcmp(n->name, "float-size") && value) {
			int percent = atoi(value);
			cfg->floatPercent = percent < 30 ? 30 : percent > 100 ? 100 : percent;
		} else if (!strcmp(n->name, "auto-focus") && value) {
			if (!_stricmp(value, "off") || !_stricmp(value, "false")) cfg->autoFocus = AUTOFOCUS_OFF;
			else if (!_stricmp(value, "lost") || !_stricmp(value, "true")) cfg->autoFocus = AUTOFOCUS_LOST;
			else if (!_stricmp(value, "always")) cfg->autoFocus = AUTOFOCUS_ALWAYS;
			else note(ctx, "general: auto-focus must be \"off\", \"lost\" or \"always\"");
		} else if (!strcmp(n->name, "debug-log")) {
			cfg->debugLog = truthy(value);
		} else if (!strcmp(n->name, "warp-cursor")) {
			cfg->warpCursor = truthy(value);
		} else if (!strcmp(n->name, "float")) {
			for (int a = 0; a < n->argc && cfg->floatRuleCount < MAX_RULES; a++) {
				cfg->floatRules[cfg->floatRuleCount++] = toWide(n->args[a]);
			}
		} else {
			note(ctx, "general: unknown option \"%s\"", n->name);
		}
	}
}

static int buttonFromName(Ctx* ctx, const char* name)
{
	if (!name || !_stricmp(name, "none")) return BTN_NONE;
	if (!_stricmp(name, "left")) return BTN_LEFT;
	if (!_stricmp(name, "right")) return BTN_RIGHT;
	if (!_stricmp(name, "middle")) return BTN_MIDDLE;
	note(ctx, "altdrag: unknown mouse button \"%s\"", name);
	return BTN_NONE;
}

static void parseAltdrag(Ctx* ctx, const KdlNode* node)
{
	Config* cfg = ctx->cfg;

	for (int i = 0; i < node->kidc; i++) {
		const KdlNode* n = node->kids[i];
		const char* value = n->argc ? n->args[0] : NULL;

		if (!strcmp(n->name, "enabled")) {
			cfg->altdragEnabled = truthy(value);
		} else if (!strcmp(n->name, "mod") && value) {
			char* expanded = expand(ctx, value);
			UINT mods = 0;
			char* copy = copyText(expanded);

			for (char* token = strtok(copy, "+"); token; token = strtok(NULL, "+")) {
				for (char* c = token; *c; c++) *c = (char)tolower((unsigned char)*c);
				UINT m = modifierFromName(token);
				if (!m) note(ctx, "altdrag: unknown modifier \"%s\"", token);
				mods |= m;
			}

			cfg->altdragMods = mods;
			free(copy);
			free(expanded);
		} else if (!strcmp(n->name, "move")) {
			cfg->altdragMoveButton = buttonFromName(ctx, value);
		} else if (!strcmp(n->name, "resize")) {
			cfg->altdragResizeButton = buttonFromName(ctx, value);
		} else {
			note(ctx, "altdrag: unknown option \"%s\"", n->name);
		}
	}

	if (cfg->altdragEnabled && cfg->altdragMods == 0) {
		note(ctx, "altdrag: needs a modifier, disabling it");
		cfg->altdragEnabled = false;
	}
}

// Expands "$mod+{1-9}" into one binding per character of the range. The number passed to
// the action is the digit (0 means 10) or the letter's position in the alphabet.
static void addRangeBindings(Ctx* ctx, const char* template, Action action)
{
	char* expanded = expand(ctx, template);
	char* open = strchr(expanded, '{');

	if (!open || !strchr(open, '}') || open[2] != '-' || open[4] != '}') {
		note(ctx, "workspaces: \"%s\" needs a range like {1-9}", template);
		free(expanded);
		return;
	}

	char low = open[1], high = open[3];
	for (char ch = low; ch <= high; ch++) {
		char combo[96];
		snprintf(combo, sizeof combo, "%.*s%c%s", (int)(open - expanded), expanded, ch, open + 5);

		UINT mods, vk;
		if (!parseCombo(combo, &mods, &vk)) {
			note(ctx, "workspaces: cannot understand key \"%s\"", combo);
			break;
		}

		int number = isdigit((unsigned char)ch) ? (ch == '0' ? 10 : ch - '0') : tolower(ch) - 'a' + 1;
		addBinding(ctx, mods, vk, action, number, NULL);
	}

	free(expanded);
}

static void parseWorkspaces(Ctx* ctx, const KdlNode* node)
{
	for (int i = 0; i < node->kidc; i++) {
		const KdlNode* n = node->kids[i];
		const char* value = n->argc ? n->args[0] : NULL;

		if (!strcmp(n->name, "follow-focus")) {
			ctx->cfg->followFocus = truthy(value);
		} else if (!strcmp(n->name, "movetype") || !strcmp(n->name, "move-type")) {
			if (value && (!_stricmp(value, "follow") || !_stricmp(value, "follows"))) ctx->cfg->moveFollows = true;
			else if (value && (!_stricmp(value, "stay") || !_stricmp(value, "not-follow") || !_stricmp(value, "not follow") ||
				!_stricmp(value, "nofollow") || !_stricmp(value, "no-follow"))) ctx->cfg->moveFollows = false;
			else note(ctx, "workspaces: movetype must be \"follow\" or \"stay\"");
		} else if (!strcmp(n->name, "send-follow") && value) {
			addRangeBindings(ctx, value, ACT_SEND_FOLLOW);
		} else if (!strcmp(n->name, "send-stay") && value) {
			addRangeBindings(ctx, value, ACT_SEND_STAY);
		} else if (!strcmp(n->name, "block-windows-shortcuts")) {
			ctx->cfg->blockWindowsDesktopKeys = truthy(value);
		} else if (!strcmp(n->name, "backend") && value) {
			if (!_stricmp(value, "auto")) ctx->cfg->desktopBackend = 0;
			else if (!_stricmp(value, "dll")) ctx->cfg->desktopBackend = 1;
			else if (!_stricmp(value, "builtin")) ctx->cfg->desktopBackend = 2;
			else note(ctx, "workspaces: backend must be \"auto\", \"dll\" or \"builtin\"");
		} else if (!strcmp(n->name, "goto") && value) {
			addRangeBindings(ctx, value, ACT_GOTO);
		} else if (!strcmp(n->name, "send") && value) {
			addRangeBindings(ctx, value, ACT_SEND);
		} else {
			note(ctx, "workspaces: unknown option \"%s\"", n->name);
		}
	}
}

static void parseBinds(Ctx* ctx, const KdlNode* node)
{
	for (int i = 0; i < node->kidc; i++) {
		const KdlNode* bind = node->kids[i];
		char* combo = expand(ctx, bind->name);
		UINT mods, vk;

		if (!parseCombo(combo, &mods, &vk)) {
			note(ctx, "binds: cannot understand key \"%s\"", combo);
			free(combo);
			continue;
		}

		if (bind->kidc < 1) {
			note(ctx, "binds: \"%s\" has no action", combo);
			free(combo);
			continue;
		}

		const KdlNode* actionNode = bind->kids[0];
		Action action = actionFromName(actionNode->name);
		if (action == ACT_NONE) {
			note(ctx, "binds: \"%s\": unknown action \"%s\"", combo, actionNode->name);
			free(combo);
			continue;
		}

		int arg = 0;
		char* text = NULL;

		if (action == ACT_SPAWN) {
			if (actionNode->argc < 1) {
				note(ctx, "binds: \"%s\": spawn needs a command", combo);
				free(combo);
				continue;
			}

			char joined[1024] = "";
			for (int a = 0; a < actionNode->argc; a++) {
				char* piece = expand(ctx, actionNode->args[a]);
				if (a) strncat(joined, " ", sizeof joined - strlen(joined) - 1);
				strncat(joined, piece, sizeof joined - strlen(joined) - 1);
				free(piece);
			}
			text = copyText(joined);
		} else if (action == ACT_GOTO || action == ACT_SEND || action == ACT_SEND_FOLLOW || action == ACT_SEND_STAY) {
			arg = actionNode->argc ? atoi(actionNode->args[0]) : 0;
			if (arg < 1) {
				note(ctx, "binds: \"%s\": %s needs a desktop number", combo, actionNode->name);
				free(combo);
				continue;
			}
		}

		addBinding(ctx, mods, vk, action, arg, text);
		free(text);
		free(combo);
	}
}

static void applyDocument(Ctx* ctx, const KdlNode* root)
{
	for (int i = 0; i < root->kidc; i++) {
		if (!strcmp(root->kids[i]->name, "vars")) parseVars(ctx, root->kids[i]);
	}

	for (int i = 0; i < root->kidc; i++) {
		const KdlNode* n = root->kids[i];
		if (!strcmp(n->name, "vars")) continue;
		else if (!strcmp(n->name, "general")) parseGeneral(ctx, n);
		else if (!strcmp(n->name, "altdrag")) parseAltdrag(ctx, n);
		else if (!strcmp(n->name, "workspaces")) parseWorkspaces(ctx, n);
		else if (!strcmp(n->name, "binds")) parseBinds(ctx, n);
		else note(ctx, "unknown section \"%s\"", n->name);
	}
}

static void setDefaults(Config* cfg)
{
	memset(cfg, 0, sizeof *cfg);
	cfg->gap = 6;
	cfg->masterPercent = 65;
	cfg->widthPresets[0] = 65;
	cfg->widthPresets[1] = 60;
	cfg->widthPresets[2] = 70;
	cfg->widthPresetCount = 3;
	cfg->floatPercent = 70;
	cfg->autoFocus = AUTOFOCUS_LOST;
	cfg->layout = LAYOUT_MASTER_STACK;
	cfg->followFocus = true;
	cfg->moveFollows = false;
	cfg->blockWindowsDesktopKeys = true;
	cfg->desktopBackend = 0;
	cfg->altdragEnabled = true;
	cfg->altdragMods = MOD_ALT;
	cfg->altdragMoveButton = BTN_LEFT;
	cfg->altdragResizeButton = BTN_RIGHT;
}

static char* readUtf8File(const wchar_t* path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	DWORD size = GetFileSize(file, NULL);
	char* text = NULL;

	if (size != INVALID_FILE_SIZE && size < (1u << 20)) {
		text = malloc(size + 1);
		DWORD read = 0;
		if (!ReadFile(file, text, size, &read, NULL)) read = 0;
		text[read] = 0;
	}

	CloseHandle(file);
	return text;
}

static void writeDefaultConfig(const wchar_t* path)
{
	wchar_t folder[MAX_PATH];
	wcsncpy(folder, path, MAX_PATH - 1), (folder)[MAX_PATH - 1] = 0;

	wchar_t* slash = wcsrchr(folder, L'\\');
	if (slash) {
		*slash = 0;
		CreateDirectoryW(folder, NULL);
	}

	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD written;
		WriteFile(file, defaultConfig, (DWORD)strlen(defaultConfig), &written, NULL);
		CloseHandle(file);
	}
}

// config.kdl next to the exe wins (portable setup), otherwise %APPDATA%\lightwm\config.kdl.
static void resolveConfigPath(void)
{
	wchar_t exeFolder[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, exeFolder, MAX_PATH);

	while (length > 0 && exeFolder[length - 1] != L'\\') length--;
	exeFolder[length] = 0;

	swprintf(configFilePath, MAX_PATH, L"%lsconfig.kdl", exeFolder);
	if (GetFileAttributesW(configFilePath) != INVALID_FILE_ATTRIBUTES) {
		return;
	}

	wchar_t appData[MAX_PATH];
	DWORD got = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	if (got == 0 || got >= MAX_PATH) {
		return;
	}

	swprintf(configFilePath, MAX_PATH, L"%ls\\lightwm\\config.kdl", appData);
}

const wchar_t* configPath(void)
{
	return configFilePath;
}

bool configLoad(Config* cfg, wchar_t* message, size_t messageCount)
{
	setDefaults(cfg);

	Ctx* ctx = calloc(1, sizeof(Ctx));
	ctx->cfg = cfg;
	setVar(ctx, "mod", "alt");

	resolveConfigPath();

	char* text = readUtf8File(configFilePath);
	if (!text) {
		writeDefaultConfig(configFilePath);
		text = copyText(defaultConfig);
	}

	char error[256];
	KdlNode* root = kdlParse(text, error, sizeof error);

	if (!root) {
		note(ctx, "%ls: %s\nUsing the built-in default config instead.", configFilePath, error);
		root = kdlParse(defaultConfig, error, sizeof error);
	}

	if (root) {
		applyDocument(ctx, root);
		kdlFree(root);
	}

	free(text);

	bool clean = ctx->log[0] == 0;
	MultiByteToWideChar(CP_UTF8, 0, ctx->log, -1, message, (int)messageCount);
	message[messageCount - 1] = 0;

	for (int i = 0; i < ctx->varCount; i++) free(ctx->vars[i].value);
	free(ctx);

	return clean;
}

void configFree(Config* cfg)
{
	for (int i = 0; i < cfg->floatRuleCount; i++) free(cfg->floatRules[i]);
	for (int i = 0; i < cfg->bindCount; i++) free(cfg->binds[i].text);
	free(cfg->binds);
	memset(cfg, 0, sizeof *cfg);
}
