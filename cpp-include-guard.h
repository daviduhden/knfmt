/*
 * Apply the IncludeGuards style to one file after tokenization. The guard name
 * is derived from the resolved path; existing guards may be rewritten or removed
 * and missing guards inserted. A path that cannot be resolved is left untouched.
 */
struct arena;
struct lexer;
struct style;

void cpp_include_guard(const struct style *, struct lexer *, struct arena *);
