/*
 * GNU annotation macros and attributes.
 */

extern int foo(const char *__restrict) __THROW;

extern int bar(const char *__restrict) __attribute__((nonnull(1))) __THROW;

void *malloc(size_t) __THROW __attribute_malloc__ __wur;

extern int baz(const char *) __attribute__((nonnull(1))) __attribute_pure__;
