/*
 * c99_static_array.
 */

void f(int a[static 10]);
void g(int a[const 10]);
void h(int n, int a[restrict n]);
