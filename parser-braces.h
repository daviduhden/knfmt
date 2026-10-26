struct doc;
struct parser;

/* Brace body is an enum, allowing enumerator syntax and alignment. */
#define PARSER_BRACES_ENUM 0x00000001u
/* Trim a blank line directly after the opening brace. */
#define PARSER_BRACES_TRIM 0x00000002u
/* Remove the given indent before emitting the right brace. */
#define PARSER_BRACES_DEDENT 0x00000004u
/* Perform conditional indentation. */
#define PARSER_BRACES_INDENT_MAYBE 0x00000008u

int parser_braces(
    struct parser *, struct doc *, struct doc *, unsigned int, unsigned int);
