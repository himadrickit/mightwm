#include "kdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char* p;
	int line;
	char* error;
	size_t errorSize;
	int failed;
} Parser;

static void fail(Parser* ps, const char* message)
{
	if (ps->failed) {
		return;
	}

	ps->failed = 1;
	if (ps->error && ps->errorSize) {
		snprintf(ps->error, ps->errorSize, "line %d: %s", ps->line, message);
	}
}

static void* grow(void* old, size_t size)
{
	void* result = realloc(old, size);
	if (!result) {
		abort();
	}
	return result;
}

static char* copyString(const char* s, size_t length)
{
	char* result = grow(NULL, length + 1);
	memcpy(result, s, length);
	result[length] = 0;
	return result;
}

static int isDelimiter(char c)
{
	return c == 0 || strchr(" \t\r\n\\/(){}<>;[]=,\"", c) != NULL;
}

static void skipBlockComment(Parser* ps)
{
	int depth = 1;
	ps->p += 2;

	while (depth > 0) {
		if (*ps->p == 0) {
			fail(ps, "unterminated block comment");
			return;
		}
		if (ps->p[0] == '/' && ps->p[1] == '*') {
			depth++;
			ps->p += 2;
		} else if (ps->p[0] == '*' && ps->p[1] == '/') {
			depth--;
			ps->p += 2;
		} else {
			if (*ps->p == '\n') ps->line++;
			ps->p++;
		}
	}
}

// Whitespace that does not end a node: spaces, block comments and line continuations.
static void skipSpace(Parser* ps)
{
	for (;;) {
		char c = *ps->p;

		if (c == ' ' || c == '\t' || c == '\r') {
			ps->p++;
		} else if (c == '/' && ps->p[1] == '*') {
			skipBlockComment(ps);
			if (ps->failed) return;
		} else if (c == '\\') {
			const char* q = ps->p + 1;
			while (*q == ' ' || *q == '\t' || *q == '\r') q++;
			if (q[0] == '/' && q[1] == '/') {
				while (*q && *q != '\n') q++;
			}
			if (*q != '\n') return;
			ps->line++;
			ps->p = q + 1;
		} else {
			return;
		}
	}
}

// Everything that may separate nodes: whitespace, newlines, semicolons and line comments.
static void skipBlank(Parser* ps)
{
	for (;;) {
		skipSpace(ps);
		if (ps->failed) return;

		char c = *ps->p;
		if (c == '\n') {
			ps->line++;
			ps->p++;
		} else if (c == ';') {
			ps->p++;
		} else if (c == '/' && ps->p[1] == '/') {
			while (*ps->p && *ps->p != '\n') ps->p++;
		} else {
			return;
		}
	}
}

static void appendUtf8(char** buffer, size_t* length, size_t* capacity, unsigned long code)
{
	char bytes[4];
	int count;

	if (code < 0x80) { bytes[0] = (char)code; count = 1; }
	else if (code < 0x800) { bytes[0] = (char)(0xC0 | (code >> 6)); bytes[1] = (char)(0x80 | (code & 0x3F)); count = 2; }
	else if (code < 0x10000) { bytes[0] = (char)(0xE0 | (code >> 12)); bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F)); bytes[2] = (char)(0x80 | (code & 0x3F)); count = 3; }
	else { bytes[0] = (char)(0xF0 | (code >> 18)); bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F)); bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F)); bytes[3] = (char)(0x80 | (code & 0x3F)); count = 4; }

	if (*length + count + 1 > *capacity) {
		*capacity = (*capacity + count + 1) * 2;
		*buffer = grow(*buffer, *capacity);
	}
	memcpy(*buffer + *length, bytes, count);
	*length += count;
}

static int readQuoted(Parser* ps, char** out)
{
	size_t length = 0, capacity = 32;
	char* buffer = grow(NULL, capacity);
	ps->p++;

	for (;;) {
		char c = *ps->p;
		if (c == 0) {
			free(buffer);
			fail(ps, "unterminated string");
			return 0;
		}
		ps->p++;

		if (c == '"') {
			break;
		}

		if (c == '\\') {
			char e = *ps->p++;
			switch (e) {
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'b': c = '\b'; break;
				case 'f': c = '\f'; break;
				case 's': c = ' '; break;
				case '"': case '\\': case '/': c = e; break;
				case 'u': {
					unsigned long code = 0;
					if (*ps->p == '{') ps->p++;
					while (*ps->p && *ps->p != '}' && *ps->p != '"') {
						char h = *ps->p++;
						code = code * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
					}
					if (*ps->p == '}') ps->p++;
					appendUtf8(&buffer, &length, &capacity, code);
					continue;
				}
				default:
					free(buffer);
					fail(ps, "unknown escape sequence in string");
					return 0;
			}
		} else if (c == '\n') {
			ps->line++;
		}

		if (length + 2 > capacity) {
			capacity *= 2;
			buffer = grow(buffer, capacity);
		}
		buffer[length++] = c;
	}

	buffer[length] = 0;
	*out = buffer;
	return 1;
}

static int readRaw(Parser* ps, const char* quote, int hashes, char** out)
{
	const char* start = quote + 1;
	const char* p = start;

	for (;;) {
		if (*p == 0) {
			fail(ps, "unterminated raw string");
			return 0;
		}
		if (*p == '"') {
			int count = 0;
			while (count < hashes && p[1 + count] == '#') count++;
			if (count == hashes) break;
		}
		if (*p == '\n') ps->line++;
		p++;
	}

	*out = copyString(start, (size_t)(p - start));
	ps->p = p + 1 + hashes;
	return 1;
}

// Reads one string, raw string or bare identifier (numbers and booleans are bare identifiers here).
static int readToken(Parser* ps, char** out)
{
	if (*ps->p == '(') {
		const char* q = ps->p;
		while (*q && *q != ')' && *q != '\n') q++;
		if (*q != ')') {
			fail(ps, "unterminated type annotation");
			return 0;
		}
		ps->p = q + 1;
	}

	const char* s = ps->p;

	if (*s == '"') {
		return readQuoted(ps, out);
	}

	const char* q = s;
	int hashes = 0;
	if (*q == 'r' && (q[1] == '"' || q[1] == '#')) q++;
	while (*q == '#') { hashes++; q++; }
	if (q != s && *q == '"') {
		return readRaw(ps, q, hashes, out);
	}

	if (*s == '#' && s[1] >= 'a' && s[1] <= 'z') s++;

	const char* e = s;
	while (!isDelimiter(*e)) e++;

	if (e == s) {
		fail(ps, "unexpected character");
		return 0;
	}

	*out = copyString(s, (size_t)(e - s));
	ps->p = e;
	return 1;
}

static void nodeAddArg(KdlNode* n, char* value)
{
	n->args = grow(n->args, sizeof(char*) * (n->argc + 1));
	n->args[n->argc++] = value;
}

static void nodeAddProp(KdlNode* n, char* key, char* value)
{
	n->props = grow(n->props, sizeof(KdlProp) * (n->propc + 1));
	n->props[n->propc].key = key;
	n->props[n->propc].value = value;
	n->propc++;
}

static void nodeAddChild(KdlNode* n, KdlNode* child)
{
	n->kids = grow(n->kids, sizeof(KdlNode*) * (n->kidc + 1));
	n->kids[n->kidc++] = child;
}

static KdlNode* newNode(void)
{
	KdlNode* n = grow(NULL, sizeof(KdlNode));
	memset(n, 0, sizeof(KdlNode));
	return n;
}

void kdlFree(KdlNode* n)
{
	if (!n) return;

	free(n->name);
	for (int i = 0; i < n->argc; i++) free(n->args[i]);
	free(n->args);
	for (int i = 0; i < n->propc; i++) { free(n->props[i].key); free(n->props[i].value); }
	free(n->props);
	for (int i = 0; i < n->kidc; i++) kdlFree(n->kids[i]);
	free(n->kids);
	free(n);
}

static void parseNodes(Parser* ps, KdlNode* parent, int depth);

static KdlNode* parseNode(Parser* ps, int depth)
{
	KdlNode* n = newNode();

	if (!readToken(ps, &n->name)) {
		kdlFree(n);
		return NULL;
	}

	for (;;) {
		skipSpace(ps);
		if (ps->failed) break;

		char c = *ps->p;
		if (c == 0 || c == '\n' || c == ';' || c == '}') {
			break;
		}

		if (c == '/' && ps->p[1] == '/') {
			while (*ps->p && *ps->p != '\n') ps->p++;
			continue;
		}

		int discard = 0;
		if (c == '/' && ps->p[1] == '-') {
			discard = 1;
			ps->p += 2;
			skipSpace(ps);
			c = *ps->p;
		}

		if (c == '{') {
			ps->p++;
			if (discard) {
				KdlNode* scratch = newNode();
				parseNodes(ps, scratch, depth + 1);
				kdlFree(scratch);
			} else {
				parseNodes(ps, n, depth + 1);
			}
			if (ps->failed) break;
			continue;
		}

		char* token = NULL;
		if (!readToken(ps, &token)) break;

		if (*ps->p == '=') {
			ps->p++;
			char* value = NULL;
			if (!readToken(ps, &value)) {
				free(token);
				break;
			}
			if (discard) { free(token); free(value); }
			else nodeAddProp(n, token, value);
		} else if (discard) {
			free(token);
		} else {
			nodeAddArg(n, token);
		}
	}

	if (ps->failed) {
		kdlFree(n);
		return NULL;
	}

	return n;
}

static void parseNodes(Parser* ps, KdlNode* parent, int depth)
{
	for (;;) {
		skipBlank(ps);
		if (ps->failed) return;

		char c = *ps->p;

		if (c == 0) {
			if (depth > 0) fail(ps, "missing closing }");
			return;
		}

		if (c == '}') {
			if (depth == 0) {
				fail(ps, "unexpected }");
				return;
			}
			ps->p++;
			return;
		}

		int discard = 0;
		if (c == '/' && ps->p[1] == '-') {
			discard = 1;
			ps->p += 2;
			skipSpace(ps);
		}

		KdlNode* n = parseNode(ps, depth);
		if (!n) return;

		if (discard) kdlFree(n);
		else nodeAddChild(parent, n);
	}
}

KdlNode* kdlParse(const char* source, char* error, size_t errorSize)
{
	Parser ps = { source, 1, error, errorSize, 0 };

	if (error && errorSize) error[0] = 0;

	if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB && (unsigned char)ps.p[2] == 0xBF) {
		ps.p += 3;
	}

	KdlNode* root = newNode();
	parseNodes(&ps, root, 0);

	if (ps.failed) {
		kdlFree(root);
		return NULL;
	}

	return root;
}

KdlNode* kdlChild(const KdlNode* node, const char* name)
{
	if (!node) return NULL;

	for (int i = 0; i < node->kidc; i++) {
		if (node->kids[i]->name && strcmp(node->kids[i]->name, name) == 0) {
			return node->kids[i];
		}
	}
	return NULL;
}

const char* kdlProp(const KdlNode* node, const char* key)
{
	if (!node) return NULL;

	for (int i = 0; i < node->propc; i++) {
		if (strcmp(node->props[i].key, key) == 0) {
			return node->props[i].value;
		}
	}
	return NULL;
}
