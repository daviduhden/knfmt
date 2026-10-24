/*
 * Differential test for the x86-64 string matching routines.
 *
 * Compares each native (SSE/AVX2/AVX-512) implementation of
 * KS_str_match()/KS_str_match_until() against the portable reference. The
 * same data is also placed against a PROT_NONE guard page so that any read
 * past the requested length faults instead of passing silently.
 *
 * Development tool; run via `bmake str-match`.
 */
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libks/capabilities.h"
#include "libks/string.h"
#include "libks/valgrind.h"

size_t	KS_str_match_native_128(const char *, size_t,
    const struct KS_str_match *);
size_t	KS_str_match_native_256(const char *, size_t,
    const struct KS_str_match *);
size_t	KS_str_match_native_512(const char *, size_t,
    const struct KS_str_match *);
size_t	KS_str_match_until_native_128(const char *, size_t,
    const struct KS_str_match *);
size_t	KS_str_match_until_native_256(const char *, size_t,
    const struct KS_str_match *);

typedef size_t (*match_fn)(const char *, size_t, const struct KS_str_match *);

static const char *const rangesets[] = {
	"  ",				/* space */
	"  \f\f\n\n\r\r\t\t\v\v",	/* whitespace, 16 bytes */
	"azAZ09",			/* alnum-ish */
	"\001\177",			/* printable ASCII */
	"~~",				/* tilde */
	"!~",				/* visible ASCII */
};

static unsigned long ntest, nfail;

static void
hexdump(const char *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		fprintf(stderr, "%02x", (unsigned char)p[i]);
}

static void
check(const char *name, const char *ranges, const char *data, size_t len,
    size_t got, size_t want)
{
	ntest++;
	if (got == want)
		return;
	nfail++;
	fprintf(stderr, "FAIL %s ranges=", name);
	hexdump(ranges, strlen(ranges));
	fprintf(stderr, " len=%zu got=%zu want=%zu data=", len, got, want);
	hexdump(data, len);
	fprintf(stderr, "\n");
}

static void
run(const char *name, const char *ranges, match_fn native, match_fn reference,
    const struct KS_str_match *match, const char *data, size_t len)
{
	check(name, ranges, data, len, native(data, len, match),
	    reference(data, len, match));
}

static void
run_one(const struct KS_str_match *match, const char *ranges,
    const char *name1, match_fn m1, const char *name2, match_fn m2)
{
	static char buf[600];
	size_t lens[] = { 0, 1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 47, 63,
	    64, 65, 95, 127, 128, 129, 200, 255 };
	size_t i, k, v;

	for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
		size_t len = lens[i];
		void *map = NULL;
		size_t maplen = 0;
		long pg = sysconf(_SC_PAGESIZE);
		char *base, *g;

		/*
		 * A pattern mixing matching and non-matching bytes: most bytes
		 * are printable ASCII, and every eighth byte is set to a value
		 * outside the printable range.
		 */
		for (k = 0; k < len; k++)
			buf[k] = (k % 8 == 0) ? (char)0x80 :
			    (char)(0x20 + (k % 95));

		/* Unguarded differential comparison. */
		if (m1 != NULL)
			run(name1, ranges, m1, KS_str_match_default, match,
			    buf, len);
		if (m2 != NULL)
			run(name2, ranges, m2, KS_str_match_until_default,
			    match, buf, len);

		/* Guarded: data ends exactly at a PROT_NONE page. */
		base = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			fprintf(stderr, "mmap: %s\n", strerror(errno));
			exit(2);
		}
		memcpy(base + pg - len, buf, len);
		if (mprotect(base + pg, (size_t)pg, PROT_NONE) == -1) {
			fprintf(stderr, "mprotect: %s\n", strerror(errno));
			exit(2);
		}
		g = base + pg - len;
		if (m1 != NULL)
			run(name1, ranges, m1, KS_str_match_default, match,
			    g, len);
		if (m2 != NULL)
			run(name2, ranges, m2, KS_str_match_until_default,
			    match, g, len);
		munmap(base, (size_t)pg * 2);

		/* Unaligned start: shift within a larger buffer. */
		for (v = 0; v < len + 3 && v < sizeof(buf); v++)
			buf[v] = (char)(0x21 + (v % 0x5e));
		for (v = 1; v <= 3 && v + len <= sizeof(buf); v++) {
			if (m1 != NULL)
				run(name1, ranges, m1, KS_str_match_default,
				    match, buf + v, len);
			if (m2 != NULL)
				run(name2, ranges, m2,
				    KS_str_match_until_default, match,
				    buf + v, len);
		}
	}
}

int
main(void)
{
	const struct KS_x86_capabilites *caps = KS_x86_capabilites();
	match_fn m128 = NULL, m256 = NULL, m512 = NULL, u128 = NULL, u256 = NULL;
	size_t i;


	if (caps == NULL) {
		printf("str-match: no x86 capabilities, skipping\n");
		return 0;
	}
	if (caps->sse == 0x42 && caps->bmi >= 1)
		m128 = KS_str_match_native_128;
	if (caps->avx >= 2 && caps->bmi >= 1)
		m256 = KS_str_match_native_256;
	if (caps->avx >= 512 && caps->avx512.bw && caps->bmi >= 1)
		m512 = KS_str_match_native_512;
	if (caps->sse == 0x42 && !KS_valgrind_is_running())
		u128 = KS_str_match_until_native_128;
	if (caps->avx >= 2 && caps->bmi >= 2)
		u256 = KS_str_match_until_native_256;

	printf("str-match: avx=%u bmi=%u sse=%#x bw=%u; testing",
	    caps->avx, caps->bmi, caps->sse, caps->avx512.bw);
	if (m128) printf(" match_128");
	if (m256) printf(" match_256");
	if (m512) printf(" match_512");
	if (u128) printf(" until_128");
	if (u256) printf(" until_256");
	printf("\n");

	if (m128 == NULL && m256 == NULL && m512 == NULL &&
	    u128 == NULL && u256 == NULL) {
		printf("str-match: no native implementation available\n");
		return 0;
	}

	/*
	 * Regression: KS_str_match_init() must terminate for a range whose
	 * upper bound is 0xff, even though the AVX2/AVX-512 nibble bitmap only
	 * represents bytes < 0x80 (all production ranges are ASCII).
	 */
	{
		struct KS_str_match match;
		char c = 'a';

		if (KS_str_match_init("\001\377", &match) == -1) {
			fprintf(stderr, "init(0x01-0xff) failed\n");
			return 2;
		}
		(void)KS_str_match_default(&c, 1, &match);
	}

	for (i = 0; i < sizeof(rangesets) / sizeof(rangesets[0]); i++) {
		struct KS_str_match match;


		if (KS_str_match_init(rangesets[i], &match) == -1) {
			fprintf(stderr, "KS_str_match_init failed\n");
			return 2;
		}
		run_one(&match, rangesets[i], "match_128", m128,
		    "until_128", u128);
		run_one(&match, rangesets[i], "match_256", m256,
		    "until_256", u256);
		run_one(&match, rangesets[i], "match_512", m512, NULL, NULL);
	}

	printf("str-match: %lu checks, %lu failures\n", ntest, nfail);
	return nfail != 0;
}
