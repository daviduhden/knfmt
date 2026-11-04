/*
 * Copyright (c) 2023 Anton Lindqvist <anton@basename.se>
 * Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h> /* mkstemp(3) on Linux */
#include <string.h>
#include <unistd.h>

#include "libks/fs.h"

static int
construct_hidden_path(const char *path, char *buf, size_t bufsiz)
{
	char prefix[PATH_MAX];
	const char *p, *suffix;
	int n;

	/* Keep the temporary beside the target so rename stays on one filesystem. */
	p = strrchr(path, '/');
	if (p != nullptr) {
		size_t len;

		len = (size_t)(&p[1] - path);
		n = snprintf(prefix, sizeof(prefix), "%.*s", (int)len, path);
		if (n < 0 || (size_t)n >= sizeof(prefix)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		suffix = &p[1];
	} else {
		prefix[0] = '\0';
		suffix = path;
	}

	n = snprintf(buf, bufsiz, "%s.%s.XXXXXX", prefix, suffix);
	if (n < 0 || (size_t)n >= bufsiz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int
open_parent_directory(const char *path)
{
	char dir[PATH_MAX];
	const char *slash;
	size_t len;

	slash = strrchr(path, '/');
	if (slash == nullptr)
		return open(".", O_RDONLY | O_CLOEXEC);

	len = (size_t)(slash - path);
	if (len == 0) {
		dir[0] = '/';
		dir[1] = '\0';
	} else {
		if (len >= sizeof(dir)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		memcpy(dir, path, len);
		dir[len] = '\0';
	}
	return open(dir, O_RDONLY | O_CLOEXEC);
}

static int
fsync_retry(int fd)
{
	while (fsync(fd) == -1) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

static int
inherit_ownership_and_permissions(int srcfd, int dstfd)
{
	struct stat dstsb, srcsb;

	if (fstat(srcfd, &srcsb) == -1)
		return -1;
	if (fstat(dstfd, &dstsb) == -1)
		return -1;

	if ((srcsb.st_uid != dstsb.st_uid || srcsb.st_gid != dstsb.st_gid) &&
	    fchown(dstfd, srcsb.st_uid, srcsb.st_gid) == -1)
		return -1;
	/* chown may clear set-id bits, so restore the mode afterwards. */
	if (fchmod(dstfd, srcsb.st_mode & 07777) == -1)
		return -1;

	return 0;
}

/*
 * Detect a replaced path or edited contents immediately before publication.
 * This is best effort: another writer can still change the path after stat().
 * Read sequentially so an input size need not be converted to off_t.
 */
static int
check_original(const char *path, int srcfd, int expectedfd, const char *original, size_t len)
{
	struct stat actual, expected, named;
	char chunk[8192];

	if (fstat(srcfd, &actual) == -1 || fstat(expectedfd, &expected) == -1 ||
	    stat(path, &named) == -1)
		return -1;
	if (actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino ||
	    actual.st_dev != named.st_dev || actual.st_ino != named.st_ino) {
		errno = EBUSY;
		return -1;
	}
	if (lseek(srcfd, 0, SEEK_SET) == -1)
		return -1;
	for (;;) {
		ssize_t nr = read(srcfd, chunk, sizeof(chunk));

		if (nr == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (nr == 0 && len == 0)
			break;
		if (nr == 0 || (size_t)nr > len || memcmp(chunk, original, (size_t)nr) != 0) {
			errno = EBUSY;
			return -1;
		}
		original += (size_t)nr;
		len -= (size_t)nr;
	}
	/* Recheck the name after reading, which can take time for a large file. */
	if (stat(path, &named) == -1)
		return -1;
	if (actual.st_dev != named.st_dev || actual.st_ino != named.st_ino) {
		errno = EBUSY;
		return -1;
	}
	return 0;
}

int
KS_fs_replace_checked(const char *path, const char *buf, size_t buflen, int expectedfd,
    const char *original, size_t original_len)
{
	char tmppath[PATH_MAX] = {0};
	mode_t old_umask;
	int dirfd = -1;
	int tmpfd = -1;
	int error = -1;
	int srcfd = -1;
	int saved_errno;

	/* Open the target so the temporary file can inherit its mode and owner. */
	srcfd = open(path, O_RDONLY | O_CLOEXEC);
	if (srcfd == -1)
		goto out;

	if (construct_hidden_path(path, tmppath, sizeof(tmppath)) == -1)
		goto out;

	old_umask = umask(0022);
	tmpfd = mkstemp(tmppath);
	umask(old_umask);
	if (tmpfd == -1)
		goto out;
	while (buflen > 0) {
		ssize_t nw;

		nw = write(tmpfd, buf, buflen);
		if (nw == -1 && errno == EINTR)
			continue;
		if (nw <= 0) {
			if (nw == 0)
				errno = EIO;
			goto out;
		}
		buf += nw;
		buflen -= (size_t)nw;
	}

	if (inherit_ownership_and_permissions(srcfd, tmpfd) == -1)
		goto out;
	if (fsync_retry(tmpfd) == -1)
		goto out;

	dirfd = open_parent_directory(path);
	if (dirfd == -1)
		goto out;
	if (expectedfd != -1 &&
	    check_original(path, srcfd, expectedfd, original, original_len) == -1)
		goto out;

	/* Rename publishes the completed contents atomically at the target path. */
	if (rename(tmppath, path) == -1)
		goto out;
	tmppath[0] = '\0';
	/* Persist the directory entry after the new file's data and metadata. */
	if (fsync_retry(dirfd) == -1)
		goto out;

	error = 0;

out:
	if (error == -1)
		saved_errno = errno;
	if (tmpfd != -1 && tmppath[0] != '\0')
		(void)unlink(tmppath);
	if (srcfd != -1)
		close(srcfd);
	if (tmpfd != -1)
		close(tmpfd);
	if (dirfd != -1)
		close(dirfd);
	if (error == -1)
		errno = saved_errno;
	return error;
}

int
KS_fs_replace(const char *path, const char *buf, size_t buflen)
{
	return KS_fs_replace_checked(path, buf, buflen, -1, nullptr, 0);
}

int
KS_fs_tmpfd(const char *buf, size_t buflen, char *path, size_t pathsiz)
{
	char tmppath[PATH_MAX];
	size_t siz = sizeof(tmppath);
	mode_t old_umask;
	int fd = -1;
	int n;

	n = snprintf(tmppath, siz, "/tmp/libks.XXXXXXXX");
	if (n < 0 || (size_t)n >= siz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	old_umask = umask(0022);
	fd = mkstemp(tmppath);
	umask(old_umask);
	if (fd == -1)
		return -1;
	/* Keep the open descriptor after removing its temporary directory entry. */
	if (unlink(tmppath) == -1)
		goto err;

	while (buflen > 0) {
		ssize_t nw;

		nw = write(fd, buf, buflen);
		if (nw == -1 && errno == EINTR)
			continue;
		if (nw <= 0) {
			if (nw == 0)
				errno = EIO;
			goto err;
		}
		buf += nw;
		buflen -= (size_t)nw;
	}
	if (lseek(fd, 0, SEEK_SET) == -1)
		goto err;

	siz = pathsiz;
	n = snprintf(path, siz, "/dev/fd/%d", fd);
	if (n < 0 || (size_t)n >= siz) {
		errno = ENAMETOOLONG;
		goto err;
	}

	return fd;

err:
	close(fd);
	return -1;
}
