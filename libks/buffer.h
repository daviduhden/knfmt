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

#ifndef LIBKS_BUFFER_H
#define LIBKS_BUFFER_H

#include <stdarg.h> /* va_list */
#include <stddef.h> /* size_t */

struct buffer;

struct buffer_callbacks {
	void *(*alloc)(size_t, void *);
	/* realloc receives both the old and requested allocation sizes. */
	void *(*realloc)(void *, size_t, size_t, void *);
	void (*free)(void *, size_t, void *);
	void *arg;
};

/* getline owns reusable storage; returned text lasts until its next call. */
struct buffer_getline {
	struct buffer *bf;
	size_t off;
};

/* Allocate a growable byte buffer with the default allocator callbacks. */
struct buffer *buffer_alloc(size_t);
/* Allocate with caller callbacks; overshoot enables geometric growth. */
struct buffer *buffer_alloc_impl(size_t, int, const struct buffer_callbacks *);
void buffer_free(struct buffer *);

/* Read a file or descriptor from its current offset through EOF. */
struct buffer *buffer_read(const char *);
struct buffer *buffer_read_fd(int);
/* Append descriptor bytes; return zero on success, one on error. */
int buffer_read_fd_impl(struct buffer *, int);

/* Transfer the backing allocation to the caller and reset the buffer. */
char *buffer_release(struct buffer *);
/* Return a NUL-terminated transferred string, adding NUL when needed. */
char *buffer_str(struct buffer *);
/* Discard the contents while retaining allocated capacity. */
void buffer_reset(struct buffer *);

/* Remove up to n trailing bytes and return the number removed. */
size_t buffer_pop(struct buffer *, size_t);

/* Return zero for equal byte sequences and nonzero when they differ. */
int buffer_cmp(const struct buffer *, const struct buffer *);

/* Append len bytes (no NUL termination is implied); return -1 on failure. */
int buffer_puts(struct buffer *, const char *, size_t);
int buffer_putc(struct buffer *, char);
int buffer_printf(struct buffer *, const char *, ...) __attribute__((format(printf, 2, 3)));
int buffer_vprintf(struct buffer *, const char *, va_list) __attribute__((format(printf, 2, 0)));

/* Return a line without its newline; NULL marks EOF or allocation error. */
const char *buffer_getline(const struct buffer *, struct buffer_getline *);
const char *buffer_getline_impl(const struct buffer *, struct buffer_getline *);
/* Release getline's reusable scratch buffer and reset its cursor. */
void buffer_getline_free(struct buffer_getline *);

const char *buffer_get_ptr(const struct buffer *);
size_t buffer_get_len(const struct buffer *);
size_t buffer_get_size(const struct buffer *);

#endif /* !LIBKS_BUFFER_H */
