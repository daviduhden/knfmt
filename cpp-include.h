struct arena;
struct arena_scope;
struct lexer;
struct options;
struct simple;
struct style;
struct token;
struct token_list;

/*
 * Include sorting state. cpp_include_add() is called for each include as the
 * lexer reads it; cpp_include_leave() flushes a run when a non-include follows,
 * and cpp_include_done() finishes the file. The object edits the prefix token
 * list in place and owns references to the includes it records.
 */
struct cpp_include *cpp_include_alloc(const struct style *, struct simple *,
    struct token_list *, struct arena *, const struct options *,
    struct arena_scope *);
void		    cpp_include_done(struct cpp_include *);
void		    cpp_include_leave(struct cpp_include *, struct lexer *);
void cpp_include_add(struct cpp_include *, struct lexer *, struct token *);
