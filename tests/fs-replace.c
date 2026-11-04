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

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libks/fs.h"

enum fault {
	NONE, WRITE_ERROR, WRITE_ZERO, WRITE_EINTR, WRITE_SHORT,
	CHMOD_ERROR, FILE_SYNC_ERROR, DIR_SYNC_ERROR, RENAME_ERROR,
	READ_ERROR, READ_EINTR, SYNC_EINTR,
};

static enum fault injected;
static int triggered;

static ssize_t
fault_write(int fd, const void *data, size_t len)
{
	if (!triggered) {
		if (injected == WRITE_ERROR || injected == WRITE_EINTR) {
			triggered = 1;
			errno = injected == WRITE_ERROR ? ENOSPC : EINTR;
			return -1;
		}
		if (injected == WRITE_ZERO) {
			triggered = 1;
			return 0;
		}
		if (injected == WRITE_SHORT && len > 1) {
			triggered = 1;
			return write(fd, data, 1);
		}
	}
	return write(fd, data, len);
}

static ssize_t
fault_read(int fd, void *data, size_t len)
{
	if (!triggered && (injected == READ_ERROR || injected == READ_EINTR)) {
		triggered = 1;
		errno = injected == READ_ERROR ? EIO : EINTR;
		return -1;
	}
	return read(fd, data, len);
}

static int
fault_fchmod(int fd, mode_t mode)
{
	if (injected == CHMOD_ERROR) {
		triggered = 1;
		errno = EPERM;
		return -1;
	}
	return fchmod(fd, mode);
}

static int
fault_fsync(int fd)
{
	struct stat sb;

	if (!triggered && injected == SYNC_EINTR) {
		triggered = 1;
		errno = EINTR;
		return -1;
	}
	if (fstat(fd, &sb) == -1)
		err(1, "fstat");
	if ((injected == DIR_SYNC_ERROR && S_ISDIR(sb.st_mode)) ||
	    (injected == FILE_SYNC_ERROR && !S_ISDIR(sb.st_mode))) {
		triggered = 1;
		errno = EIO;
		return -1;
	}
	return fsync(fd);
}

static int
fault_rename(const char *from, const char *to)
{
	if (injected == RENAME_ERROR) {
		triggered = 1;
		errno = EACCES;
		return -1;
	}
	return rename(from, to);
}

/* Compile the real implementation with deterministic, test-only I/O faults. */
#define read fault_read
#define write fault_write
#define fchmod fault_fchmod
#define fsync fault_fsync
#define rename fault_rename
#include "../libks/fs.c"
#undef read
#undef write
#undef fchmod
#undef fsync
#undef rename

static void
require(int good, const char *message)
{
	if (!good)
		errx(1, "%s", message);
}

static int
create(const char *path, const char *text)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	size_t len = strlen(text);

	if (fd == -1 || write(fd, text, len) != (ssize_t)len)
		err(1, "create %s", path);
	return fd;
}

static void
check_text(const char *path, const char *want)
{
	char got[128];
	int fd = open(path, O_RDONLY);
	ssize_t len;

	if (fd == -1)
		err(1, "open %s", path);
	len = read(fd, got, sizeof(got));
	require(len >= 0 && (size_t)len == strlen(want) &&
	    memcmp(got, want, (size_t)len) == 0, "unexpected file contents");
	close(fd);
}

static void
check_directory(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *entry;
	unsigned int count = 0;

	if (dir == nullptr)
		err(1, "opendir");
	while ((entry = readdir(dir)) != nullptr) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			count++;
	}
	closedir(dir);
	require(count == 1, "temporary file leaked");
}

int
main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	char directory[PATH_MAX], path[PATH_MAX], moved[PATH_MAX];
	int n;

	n = snprintf(directory, sizeof(directory), "%s/knfmt-fs.XXXXXX",
	    tmpdir != nullptr ? tmpdir : "/tmp");
	require(n > 0 && (size_t)n < sizeof(directory), "temporary path too long");
	if (mkdtemp(directory) == nullptr)
		err(1, "mkdtemp");
	n = snprintf(path, sizeof(path), "%s/input.c", directory);
	require(n > 0 && (size_t)n < sizeof(path), "input path too long");
	n = snprintf(moved, sizeof(moved), "%s/moved.c", directory);
	require(n > 0 && (size_t)n < sizeof(moved), "moved path too long");

	for (enum fault f = NONE; f <= SYNC_EINTR; f++) {
		struct stat sb;
		int fd = create(path, "original");
		int rv;
		bool success = f == NONE || f == WRITE_EINTR || f == WRITE_SHORT ||
		    f == READ_EINTR || f == SYNC_EINTR;

		injected = f;
		triggered = 0;
		rv = KS_fs_replace_checked(path, "formatted", 9, fd, "original", 8);
		require(success ? rv == 0 : rv == -1, "unexpected replacement status");
		require(f == NONE || triggered, "fault was not exercised");
		check_text(path, success || f == DIR_SYNC_ERROR ? "formatted" : "original");
		require(stat(path, &sb) == 0 && (sb.st_mode & 07777) == 0600,
		    "mode was not preserved");
		close(fd);
		check_directory(directory);
	}
	injected = NONE;

	/* An edit to the same inode must not be overwritten. */
	int fd = create(path, "original");
	require(pwrite(fd, "edited!!", 8, 0) == 8, "edit failed");
	require(KS_fs_replace_checked(path, "formatted", 9, fd, "original", 8) == -1 &&
	    errno == EBUSY, "concurrent edit was not detected");
	check_text(path, "edited!!");
	close(fd);
	check_directory(directory);

	/* Same bytes at a different inode are a different input. */
	fd = create(path, "original");
	require(rename(path, moved) == 0, "move failed");
	int newer = create(path, "original");
	close(newer);
	require(KS_fs_replace_checked(path, "formatted", 9, fd, "original", 8) == -1 &&
	    errno == EBUSY, "replaced inode was not detected");
	check_text(path, "original");
	close(fd);
	require(unlink(moved) == 0, "unlink moved failed");
	check_directory(directory);

	/* Binary snapshots cross read chunks; edits, append and truncate all fail. */
	for (int change = 0; change < 4; change++) {
		char original[20000];
		struct stat sb;

		memset(original, 'x', sizeof(original));
		original[42] = '\0';
		fd = create(path, "");
		require(write(fd, original, sizeof(original)) == (ssize_t)sizeof(original),
		    "binary input failed");
		if (change == 1)
			require(write(fd, "!", 1) == 1, "append failed");
		else if (change == 2)
			require(ftruncate(fd, (off_t)sizeof(original) - 1) == 0, "truncate failed");
		else if (change == 3)
			require(pwrite(fd, "!", 1, 9000) == 1, "middle edit failed");
		require(lseek(fd, 123, SEEK_SET) == 123, "seek input failed");
		int rv = KS_fs_replace_checked(path, "formatted", 9, fd,
		    original, sizeof(original));
		require(change == 0 ? rv == 0 : rv == -1 && errno == EBUSY,
		    "binary snapshot status incorrect");
		require(lseek(fd, 0, SEEK_CUR) == 123, "input descriptor offset changed");
		if (change == 0) {
			check_text(path, "formatted");
		} else {
			require(stat(path, &sb) == 0 && sb.st_size ==
			    (off_t)sizeof(original) + (change == 1 ? 1 : change == 2 ? -1 : 0),
			    "concurrent binary edit lost");
			char byte;
			require(pread(fd, &byte, 1, change == 3 ? 9000 : 42) == 1 &&
			    byte == (change == 3 ? '!' : '\0'), "binary contents changed");
		}
		close(fd);
		check_directory(directory);
	}

	/* Historical symlink replacement and hard-link separation remain explicit. */
	require(unlink(path) == 0, "unlink input failed");
	fd = create(moved, "original");
	require(symlink("moved.c", path) == 0, "symlink failed");
	require(KS_fs_replace_checked(path, "formatted", 9, fd, "original", 8) == 0,
	    "symlink replacement failed");
	check_text(path, "formatted");
	check_text(moved, "original");
	close(fd);
	require(unlink(path) == 0, "unlink replacement failed");
	require(link(moved, path) == 0, "link failed");
	fd = open(path, O_RDONLY);
	require(fd != -1 && KS_fs_replace_checked(path, "formatted", 9, fd, "original", 8) == 0,
	    "hard-link replacement failed");
	check_text(moved, "original");
	close(fd);

	require(unlink(path) == 0 && unlink(moved) == 0 && rmdir(directory) == 0,
	    "cleanup failed");
	puts("fs replacement: I/O faults, concurrent edits, modes and links passed");
	return 0;
}
