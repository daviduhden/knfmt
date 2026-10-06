/*
 * c23_va_opt.
 */

#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)
void f(int, ...);
