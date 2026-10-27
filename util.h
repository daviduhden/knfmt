#include <stddef.h> /* size_t */

/*
 * Small formatting helpers shared by the lexer, renderer and ruler. Columns are
 * 1-based, tabs stop every eight columns, and all length parameters are explicit
 * so NUL bytes in token text remain representable.
 */
struct arena_scope;
struct buffer;

const char *path_slice(const char *, unsigned int, struct arena_scope *);

unsigned int colwidth(const char *, size_t, unsigned int);
size_t strwidth(const char *, size_t, size_t);

size_t strindent_buffer(struct buffer *, size_t, int, size_t);
