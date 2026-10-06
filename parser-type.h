struct doc;
struct parser;
struct ruler;

#define PARSER_TYPE_CAST		0x00000001U
#define PARSER_TYPE_ARG			0x00000002U
#define PARSER_TYPE_EXPR		0x00000004U

struct parser_type {
	struct token	*beg;
	struct token	*end;
	/* Optional token to insert ruler alignment after. */
	struct token	*align;
	/* Optional token denoting start of arguments for function pointers. */
	struct token	*args;
	/*
	 * Non-zero when the whole declarator of a function definition or
	 * declaration is described by beg..end, e.g.
	 * int (*const f(void))[10]. parser_type() then renders the entire
	 * declarator and parser_func_proto() must not render it again.
	 */
	unsigned int	 func_decl:1;
};

int	parser_type_peek(struct parser *, struct parser_type *, unsigned int);
int	parser_type_func_declarator(struct parser *, struct parser_type *);
int	parser_type(struct parser *, struct doc *, struct parser_type *,
    struct ruler *);

/* Implicit int (C89/C90) classification. */
#define PARSER_IMPLICIT_NONE	0
#define PARSER_IMPLICIT_DECL	1
#define PARSER_IMPLICIT_IMPL	2

int	parser_type_implicit_int(struct parser *);
int	parser_type_implicit_int_func(struct parser *);
int	parser_type_decl_list_then_lbrace(struct parser *);
