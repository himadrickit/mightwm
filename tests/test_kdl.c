#include "../kdl.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
	char err[256];
	const char* doc =
		"\xEF\xBB\xBF// leading comment\n"
		"vars { mod \"alt\"; term \"wt.exe\" }\n"
		"general {\n"
		"    gap 6\n"
		"    layout \"master-stack\"   // trailing comment\n"
		"    float \"calc.exe\" r\"C:\\raw path\\x.exe\" r#\"has \"quotes\"\"#\n"
		"    /- ignored 1 2 3\n"
		"}\n"
		"/* block /* nested */ comment */\n"
		"binds {\n"
		"    $mod+Return { spawn \"%LOCALAPPDATA%\\\\wt.exe\" \"-d\" \"x\" }\n"
		"    $mod+shift+q { close-window }\n"
		"    alt+f  { fullscreen }; alt+m { monocle }\n"
		"}\n"
		"altdrag enabled=true mod=\"alt\" {\n"
		"    move \"left\"\n"
		"    /- resize \"right\"\n"
		"    resize \"middle\" extra=#true \\\n"
		"        more\n"
		"}\n"
		"uni \"snow\\u{2603}man\" \"a\\tb\"\n";

	KdlNode* root = kdlParse(doc, err, sizeof err);
	CHECK(root != NULL);
	if (!root) { printf("%s\n", err); return 1; }

	KdlNode* vars = kdlChild(root, "vars");
	CHECK(vars && vars->kidc == 2 && strcmp(vars->kids[1]->args[0], "wt.exe") == 0);

	KdlNode* general = kdlChild(root, "general");
	CHECK(general && general->kidc == 3);
	KdlNode* fl = kdlChild(general, "float");
	CHECK(fl && fl->argc == 3);
	CHECK(strcmp(fl->args[1], "C:\\raw path\\x.exe") == 0);
	CHECK(strcmp(fl->args[2], "has \"quotes\"") == 0);
	CHECK(strcmp(kdlChild(general, "gap")->args[0], "6") == 0);

	KdlNode* binds = kdlChild(root, "binds");
	CHECK(binds && binds->kidc == 4);
	CHECK(strcmp(binds->kids[0]->name, "$mod+Return") == 0);
	CHECK(strcmp(binds->kids[0]->kids[0]->name, "spawn") == 0 && binds->kids[0]->kids[0]->argc == 3);
	CHECK(strcmp(binds->kids[0]->kids[0]->args[0], "%LOCALAPPDATA%\\wt.exe") == 0);
	CHECK(strcmp(binds->kids[3]->name, "alt+m") == 0);

	KdlNode* drag = kdlChild(root, "altdrag");
	CHECK(drag && strcmp(kdlProp(drag, "enabled"), "true") == 0 && strcmp(kdlProp(drag, "mod"), "alt") == 0);
	CHECK(drag->kidc == 2 && strcmp(drag->kids[1]->args[0], "middle") == 0);
	CHECK(strcmp(kdlProp(drag->kids[1], "extra"), "true") == 0 && drag->kids[1]->argc == 2);

	KdlNode* uni = kdlChild(root, "uni");
	CHECK(uni && strcmp(uni->args[0], "snow\xE2\x98\x83man") == 0 && strcmp(uni->args[1], "a\tb") == 0);
	kdlFree(root);

	const char* bad[] = { "a {", "a }", "a \"unterminated", "/* open", "a b=", "a {\n b {\n}\n" };
	for (int i = 0; i < 6; i++) {
		KdlNode* r = kdlParse(bad[i], err, sizeof err);
		CHECK(r == NULL);
		if (r) kdlFree(r);
		else printf("  expected error [%d]: %s\n", i, err);
	}

	printf("kdl: %d failures\n", failures);
	return failures != 0;
}
