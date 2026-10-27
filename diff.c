#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <limits.h> /* PATH_MAX */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "diff.h"
#include "file.h"
#include "libks/arena-buffer.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/compiler.h"
#include "libks/vector.h"
#include "options.h"
#include "path.h"
#include "trace-types.h"
#include "trace.h"

static void diff_end(struct diffchunk *, unsigned int);

static int matchpath(const char *, const char **, struct arena_scope *);
static int matchchunk(const char *, unsigned int *, unsigned int *);
static int matchline(const char *, unsigned int, struct file *);

static const char *trimprefix(const char *, size_t *);

static regex_t rechunk, repath;

/*
 * Number of directories above the current working directory where a Git
 * repository resides. Used to adjust diff paths when invoking knfmt from a
 * directory nested below the repository root.
 */
static int git_ndirs;

/*
 * Compile the regular expressions used to scan a unified diff and determine how
 * many directories above the working directory contain a Git repository. The
 * latter lets -D resolve the repository-relative paths emitted by git diff.
 */
void
diff_init(void)
{
	struct {
		regex_t *r;
		const char *p;
	} patterns[] = {
	    {&rechunk, "^@@.+\\+([[:digit:]]+)(,([[:digit:]]+))?.+@@"},
	    {&repath, "^\\+\\+\\+[[:space:]]+([^[:space:]]+)"},
	};
	size_t i;
	int fd;

	for (i = 0; i < countof(patterns); i++) {
		char errbuf[128];
		int error;

		error = regcomp(patterns[i].r, patterns[i].p, REG_EXTENDED | REG_NEWLINE);
		if (error == 0)
			continue;

		if (regerror(error, patterns[i].r, errbuf, sizeof(errbuf)) > 0)
			errx(1, "regcomp: %s", errbuf);
	}

	fd = searchpath(".git", &git_ndirs);
	if (fd != -1)
		close(fd);
}

void
diff_shutdown(void)
{
	regfree(&rechunk);
	regfree(&repath);
}

/*
 * Build the file list from a unified diff on standard input. A "+++" header
 * introduces a file; each "@@" header describes the range of lines in the new
 * file that the hunk touches. Added lines ("+") start or extend a chunk, context
 * lines close it, and removed lines do not advance the new-file line number.
 * Only the new-file line numbers are used, and the diff is trusted to be
 * consistent with the source.
 */
int
diff_parse(struct files *files, struct arena_scope *eternal_scope, struct arena *scratch,
    const struct options *op)
{
	struct buffer *bf;
	struct buffer_getline it = {0};
	struct file *fe = nullptr;
	const char *line;
	int error = 0;

	arena_scope(scratch, s);

	bf = arena_buffer_read(&s, "/dev/stdin");
	if (bf == nullptr)
		return 1;

	while ((line = arena_buffer_getline(&s, bf, &it)) != nullptr) {
		const char *path;
		unsigned int el, sl;

		if (matchpath(line, &path, &s)) {
			fe = files_alloc(files, path, eternal_scope);
		} else if (matchchunk(line, &sl, &el)) {
			/* Chunks cannot be present before the path. */
			if (fe == nullptr) {
				error = 1;
				goto out;
			}

			while (sl <= el) {
				line = arena_buffer_getline(&s, bf, &it);
				if (line == nullptr) {
					error = 1;
					goto out;
				}

				if (matchline(line, sl, fe))
					sl++;
			}

			diff_end(fe->fe_diff, el);
		}
	}

	if (options_trace_level(op, TRACE_DIFF) > 0) {
		size_t i;

		for (i = 0; i < VECTOR_LENGTH(files->fs_vc); i++) {
			size_t j;

			fe = &files->fs_vc[i];
			for (j = 0; j < VECTOR_LENGTH(fe->fe_diff); j++) {
				const struct diffchunk *du = &fe->fe_diff[j];

				trace(TRACE_DIFF, op, "%s: %u-%u", fe->fe_path, du->du_beg,
				    du->du_end);
			}
		}
	}

out:
	return error;
}

/*
 * Return the chunk containing the given 1-based line number, or nullptr if the
 * line is not part of any changed region. Chunks are searched linearly; the
 * vector is small and only consulted while rendering in -D mode.
 */
const struct diffchunk *
diff_get_chunk(const struct diffchunk *chunks, unsigned int lno)
{
	size_t i;

	for (i = 0; i < VECTOR_LENGTH(chunks); i++) {
		const struct diffchunk *du = &chunks[i];

		if (lno >= du->du_beg && lno <= du->du_end)
			return du;
	}
	return nullptr;
}

/*
 * Finish the most recently created chunk by recording its last line. Chunks
 * are created with du_end == 0 and closed here; an already closed chunk is left
 * untouched.
 */
static void
diff_end(struct diffchunk *chunks, unsigned int lno)
{
	struct diffchunk *du;

	du = VECTOR_LAST(chunks);
	if (du != nullptr && du->du_end == 0)
		du->du_end = lno;
}

/*
 * Extract the new-file path from a "+++" header. Git's "b/" prefix is trimmed,
 * and if the resulting path does not exist the leading components are dropped
 * one at a time to compensate for running knfmt below the repository root.
 * The returned string is allocated in the caller's scratch scope.
 */
static int
matchpath(const char *str, const char **out, struct arena_scope *s)
{
	regmatch_t rm[2];
	struct stat sb;
	const char *buf, *path;
	size_t len;

	if (regexec(&repath, str, 2, rm, 0))
		return 0;

	buf = str + rm[1].rm_so;
	len = (size_t)(rm[1].rm_eo - rm[1].rm_so);
	if (strncmp(buf, "b/", 2) == 0) {
		/* Trim git prefix. */
		buf = trimprefix(buf, &len);
	}
	path = arena_strndup(s, buf, len);

	/* Try to adjust Git repository relative path(s). */
	if (git_ndirs > 0 && path[0] != '/' && stat(path, &sb) == -1 && errno == ENOENT) {
		int ntrim;

		for (ntrim = git_ndirs; ntrim > 0; ntrim--)
			buf = trimprefix(buf, &len);
		path = arena_strndup(s, buf, len);
	}

	*out = path;
	return 1;
}

static unsigned int
strtou(const char *str)
{
	unsigned long n;

	errno = 0;
	n = strtoul(str, nullptr, 10);
	if (n == 0 || n > UINT_MAX || errno != 0)
		return 0;
	return (unsigned int)n;
}

/*
 * Decode an "@@" hunk header into the first and last new-file line it covers.
 * A missing line count denotes a single line. Returns zero for malformed or
 * overflowing ranges.
 */
static int
matchchunk(const char *str, unsigned int *sl, unsigned int *el)
{
	regmatch_t rm[4];
	unsigned int lno;

	if (regexec(&rechunk, str, 4, rm, 0))
		return 0;

	lno = strtou(str + rm[1].rm_so);
	if (lno == 0)
		return 0;
	*sl = lno;

	if (rm[3].rm_so != -1 && rm[3].rm_eo != -1) {
		lno = strtou(str + rm[3].rm_so);
		if (lno == 0)
			return 0;
		if (lno > UINT_MAX - *sl + 1)
			return 0;
		*el = (*sl + lno) - 1;
	} else {
		*el = *sl;
	}

	return 1;
}

/*
 * Account for one line inside a hunk. Added lines start a chunk when the
 * current one is closed; context lines close it, and removed lines neither
 * start nor close a chunk. Returns non-zero if the line advances the new-file
 * line number; removed lines do not.
 */
static int
matchline(const char *str, unsigned int lno, struct file *fe)
{
	struct diffchunk *du;

	if (str[0] == '-')
		return 0;

	du = VECTOR_LAST(fe->fe_diff);
	if (str[0] == '+') {
		if (du == nullptr || (du->du_beg > 0 && du->du_end > 0)) {
			du = VECTOR_CALLOC(fe->fe_diff);
			if (du == nullptr)
				err(1, nullptr);
			du->du_beg = lno;
		}
	} else {
		diff_end(fe->fe_diff, lno - 1);
	}
	return 1;
}

/*
 * Drop the first path component from the length-delimited string. Returns the
 * original pointer when there is no separator, otherwise the remainder after
 * the first slash and the adjusted length.
 */
static const char *
trimprefix(const char *str, size_t *len)
{
	const char *p;

	p = memchr(str, '/', *len);
	if (p == nullptr)
		return str;
	*len -= (size_t)((p - str) + 1);
	return &p[1];
}
