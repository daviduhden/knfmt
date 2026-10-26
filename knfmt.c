#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "arenas.h"
#include "clang.h"
#include "config.h"
#include "diff.h"
#include "expr.h"
#include "file.h"
#include "lexer.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/exec.h"
#include "libks/fs.h"
#include "libks/vector.h"
#include "options.h"
#include "parser.h"
#include "simple.h"
#include "style.h"
#include "trace-types.h"

/*
 * Process-wide state shared by every input file. The arenas and the parsed
 * style outlive all files; src and dst are reused for each file and reset
 * between them. The simple object carries the state of the -s passes.
 */
struct main_context {
	struct options options;
	struct style  *style;
	struct simple *simple;
	struct buffer *src;
	struct buffer *dst;
	struct arenas  arena;
};

[[noreturn]] static void usage(void);

static int filelist(int, char **, struct files *, struct arena_scope *,
    struct arena *, const struct options *);
static int fileformat(struct main_context *, struct file *);
static int filediff(struct main_context *, const struct file *);
static int filewrite(struct main_context *, const struct file *);
static int fileprint(const struct buffer *);

/*
 * Command line entry point. Options are parsed and the shared keyword, style
 * and expression tables are initialized before any input is read. Style parsing
 * may execute a helper program, so pledge(2) is delayed until afterwards.
 * Files are then formatted one at a time using the same buffers and arenas;
 * a failure in one file does not prevent the remaining files from being
 * processed, but makes the final exit status non-zero.
 */
int
main(int argc, char *argv[])
{
	struct main_context c = {0};
	struct files	    files = {0};
	const char	   *clang_format = nullptr;
	size_t		    i;
	int		    error = 0;
	int		    ch;

	/* Parsing a style may execute a helper; pledge only after it finishes.
	 */

	options_init(&c.options);

	while ((ch = getopt(argc, argv, "c:Ddist:")) != -1) {
		switch (ch) {
		case 'c':
			clang_format = optarg;
			break;
		case 'D':
			c.options.diffparse = 1;
			break;
		case 'd':
			c.options.diff = 1;
			break;
		case 'i':
			c.options.inplace = 1;
			break;
		case 's':
			c.options.simple = 1;
			break;
		case 't':
			if (options_trace_parse(&c.options, optarg))
				return 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if ((c.options.diffparse && argc > 0) ||
	    (!c.options.diffparse && c.options.inplace && argc == 0))
		usage();

	clang_init();
	expr_init();
	style_init();
	if (c.options.diffparse)
		diff_init();

	if (VECTOR_INIT(files.fs_vc))
		err(1, nullptr);

	arenas_init(&c.arena);
	arena_scope(c.arena.eternal, eternal_scope);
	arena_scope(c.arena.buffer, buffer_scope);
	c.style = style_parse(
	    clang_format, &eternal_scope, c.arena.scratch, &c.options);
	if (c.style == nullptr) {
		error = 1;
		goto out;
	}

	/* Diff mode executes diff(1); other modes can now be pledged. */
	if (!c.options.diff) {
		const char *promises = c.options.inplace ?
		    "stdio rpath wpath cpath fattr chown" :
		    "stdio rpath";
		if (pledge(promises, nullptr) == -1)
			err(1, "pledge");
	}

	c.simple = simple_alloc(&eternal_scope, &c.options);

	c.src = arena_buffer_alloc(&buffer_scope, 1 << 12);
	c.dst = arena_buffer_alloc(&buffer_scope, 1 << 12);

	if (filelist(argc, argv, &files, &eternal_scope, c.arena.scratch,
		&c.options)) {
		error = 1;
		goto out;
	}

	for (i = 0; i < VECTOR_LENGTH(files.fs_vc); i++) {
		struct file *fe = &files.fs_vc[i];

		if (fileformat(&c, fe))
			error = 1;
		buffer_reset(c.src);
		buffer_reset(c.dst);
		file_close(fe);
	}

out:
	files_free(&files);
	arenas_free(&c.arena);
	style_shutdown();
	expr_shutdown();
	clang_shutdown();
	if (c.options.diffparse)
		diff_shutdown();

	return error;
}

static void
usage(void)
{
	fprintf(stderr, "usage: knfmt [-dis] [-c file] [-t flags] [file ...]\n");
	fprintf(stderr, "       knfmt -D [-dis] [-c file] [-t flags]\n");
	exit(1);
}

/*
 * Populate the list of files to process. In -D mode the list is derived from
 * the unified diff read from standard input; otherwise each command line
 * argument is appended, or /dev/stdin when no arguments were given.
 */
static int
filelist(int argc, char **argv, struct files *files,
    struct arena_scope *eternal_scope, struct arena *scratch,
    const struct options *op)
{
	if (op->diffparse)
		return diff_parse(files, eternal_scope, scratch, op);

	if (argc == 0) {
		files_alloc(files, "/dev/stdin", eternal_scope);
	} else {
		int i;

		for (i = 0; i < argc; i++)
			files_alloc(files, argv[i], eternal_scope);
	}
	return 0;
}

/*
 * Format one buffer. A fresh clang, lexer and parser are created in the given
 * scope because the lexer owns the token list for exactly one input; the style,
 * options, simplifier and arenas are shared through main_context. The parser
 * writes the rendered result to out and returns non-zero on a rejected input,
 * in which case out must not be used.
 */
static int
format_buffer(struct main_context *c, struct file *fe, const struct buffer *in,
    struct buffer *out, struct arena_scope *scope)
{
	struct clang  *clang;
	struct lexer  *lx;
	struct parser *pr;

	clang = clang_alloc(
	    c->style, c->simple, &c->arena, fe->fe_diff, &c->options, scope);
	lx = lexer_tokenize(&(const struct lexer_arg){
	    .path = fe->fe_path,
	    .bf = in,
	    .op = &c->options,
	    .error_flush = options_trace_level(&c->options, TRACE_LEXER) > 0,
	    .arena =
		{
		    .eternal_scope = scope,
		    .scratch = c->arena.scratch,
		},
	    .callbacks = clang_lexer_callbacks(clang),
	});
	if (lx == nullptr)
		return 1;
	if (options_trace_level(&c->options, TRACE_TOKEN) > 0)
		lexer_dump(lx);

	pr = parser_alloc(
	    &(struct parser_arg){
		.lexer = lx,
		.options = &c->options,
		.style = c->style,
		.simple = c->simple,
		.clang = clang,
		.arena = &c->arena,
	    },
	    scope);
	return parser_exec(pr, fe->fe_diff, out);
}

/*
 * Read, format and deliver one file. The input and output buffers are owned by
 * main_context and are reset by the caller for the next file. -d, -i and plain
 * output are mutually exclusive modes selected by the options.
 */
static int
fileformat(struct main_context *c, struct file *fe)
{
	arena_scope(c->arena.eternal, eternal_scope);

	if (file_read(fe, c->src))
		return 1;

	if (format_buffer(c, fe, c->src, c->dst, &eternal_scope))
		return 1;

	if (c->options.diff)
		return filediff(c, fe);
	if (c->options.inplace)
		return filewrite(c, fe);
	return fileprint(c->dst);
}

/*
 * Emit a unified diff between the source and the formatted result. An
 * identical buffer produces no output and success. diff(1) is spawned through
 * libks; a warning is printed if it cannot be executed.
 */
static int
filediff(struct main_context *c, const struct file *fe)
{
	int error;

	if (buffer_cmp(c->src, c->dst) == 0)
		return 0;

	error = KS_exec_diff(fe->fe_path, buffer_get_ptr(c->src),
	    buffer_get_len(c->src), buffer_get_ptr(c->dst),
	    buffer_get_len(c->dst));
	if (error == -1)
		warn("%s", fe->fe_path);
	return error;
}

/*
 * Replace the input file with the formatted result. Nothing is written when
 * the bytes are unchanged. KS_fs_replace() writes a sibling temporary file and
 * renames it over the target, so a failed run leaves the original intact.
 */
static int
filewrite(struct main_context *c, const struct file *fe)
{
	const struct buffer *src = c->src;
	const struct buffer *dst = c->dst;

	if (buffer_cmp(src, dst) == 0)
		return 0;
	if (KS_fs_replace(
		fe->fe_path, buffer_get_ptr(dst), buffer_get_len(dst)) == -1) {
		warn("%s", fe->fe_path);
		return 1;
	}
	return 0;
}

/*
 * Write the formatted buffer to standard output, retrying short writes. A
 * write(2) failure is reported and makes the file fail.
 */
static int
fileprint(const struct buffer *dst)
{
	const char *buf = buffer_get_ptr(dst);
	size_t	    buflen = buffer_get_len(dst);

	while (buflen > 0) {
		ssize_t nw;

		nw = write(1, buf, buflen);
		if (nw == -1) {
			warn("write: /dev/stdout");
			return 1;
		}
		buflen -= (size_t)nw;
		buf += nw;
	}
	return 0;
}
