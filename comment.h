#include <stddef.h>

struct arena;
struct arena_scope;
struct style;
struct token;

/*
 * Return a reindented, right-trimmed copy of a multi-line comment, or nullptr if
 * no change is needed. The explicit length preserves embedded NUL bytes.
 */
const char *comment_trim(const struct token *, const struct style *,
    struct arena *, struct arena_scope *, size_t *);
