#pragma once

#include <stddef.h>

typedef struct {
	char* key;
	char* value;
} KdlProp;

typedef struct KdlNode KdlNode;

struct KdlNode {
	char* name;
	char** args;
	int argc;
	KdlProp* props;
	int propc;
	KdlNode** kids;
	int kidc;
};

// Parses a KDL document. Returns a root node (name == NULL) holding the top level nodes, or NULL on error.
// All values are returned as strings: quoted/raw strings are unescaped, numbers/booleans keep their text.
KdlNode* kdlParse(const char* source, char* error, size_t errorSize);
void kdlFree(KdlNode* node);
KdlNode* kdlChild(const KdlNode* node, const char* name);
const char* kdlProp(const KdlNode* node, const char* key);
