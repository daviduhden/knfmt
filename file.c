#include <err.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"
#include "diff.h"
#include "file.h"
#include "libks/arena.h"
#include "libks/buffer.h"
#include "libks/vector.h"

/*
 * Append a file entry to the list. The path is copied into the eternal scope so
 * that it remains valid for the whole run; the entry owns its diff-chunk
 * vector and, once file_read() succeeds, an open file descriptor. Allocation
 * failure is fatal.
 */
struct file *
files_alloc(
    struct files *files, const char *path, struct arena_scope *eternal_scope)
{
	struct file *fe;

	fe = VECTOR_CALLOC(files->fs_vc);
	if (fe == nullptr)
		err(1, nullptr);
	fe->fe_path = arena_strdup(eternal_scope, path);
	if (VECTOR_INIT(fe->fe_diff))
		err(1, nullptr);
	fe->fe_fd = -1;
	return fe;
}

/*
 * Close and release every file entry, including the vector itself. Descriptors
 * are closed through file_close() so an already closed entry is harmless.
 */
void
files_free(struct files *files)
{
	while (!VECTOR_EMPTY(files->fs_vc)) {
		struct file *fe;

		fe = VECTOR_POP(files->fs_vc);
		VECTOR_FREE(fe->fe_diff);
		file_close(fe);
	}
	VECTOR_FREE(files->fs_vc);
}

/*
 * Read the whole file into bf and remember the descriptor for later closing.
 * On failure the caller's buffer is not assumed to hold usable data and the
 * descriptor is closed immediately; the path is included in the warning.
 */
int
file_read(struct file *fe, struct buffer *bf)
{
	int fd;

	fd = open(fe->fe_path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		goto err;
	if (buffer_read_fd_impl(bf, fd))
		goto err;
	fe->fe_fd = fd;
	return 0;

err:
	warn("%s", fe->fe_path);
	if (fd != -1)
		close(fd);
	return 1;
}

/*
 * Close the file descriptor if one is still open. Idempotent; the entry keeps
 * its path and diff chunks.
 */
void
file_close(struct file *fe)
{
	if (fe->fe_fd == -1)
		return;
	close(fe->fe_fd);
	fe->fe_fd = -1;
}
