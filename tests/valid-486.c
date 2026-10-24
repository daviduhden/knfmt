/*
 * decl_stress.
 */

int (*(*f(void))[3])(int);
int *(*(*g)[4])(void);
void h(int (*)(void));
void i(int (*)[3]);
void j(int (*[2])(void));
