#include <stddef.h>

struct arenas;
struct lexer;
struct options;
struct style;
struct token;

/*
 * Format a multiline preprocessor directive. Returns the replacement text and
 * its length through lenp, or nullptr when the token should keep its source
 * spelling. The returned string is allocated in the lexer's arena.
 */
const char *cpp_format(const struct lexer *, struct token *, const struct style *, struct arenas *,
    const struct options *, size_t *);
