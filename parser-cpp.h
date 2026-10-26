struct doc;
struct parser;
struct parser_type;
struct token;

/* The declaration is at file scope, where a bare macro followed by ';' is valid. */
#define PARSER_CPP_DECL_ROOT 0x00000001u

int parser_cpp_peek_type(struct parser *, struct token **);
int parser_cpp_peek_decl(struct parser *, struct parser_type *, unsigned int);
int parser_cpp_peek_x(struct parser *, struct token **);
int parser_cpp_x(struct parser *, struct doc *);
int parser_cpp_cdefs(struct parser *, struct doc *);
int parser_cpp_decl_root(struct parser *, struct doc *);
