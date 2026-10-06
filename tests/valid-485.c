/*
 * cpp_heavy.
 */

#define A(x) ((x) + 1)
#define B(x, ...) f((x), __VA_ARGS__)
#if defined(A)
int x = A(1);
#else
int x = 0;
#endif
