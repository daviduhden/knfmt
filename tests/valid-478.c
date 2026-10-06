/*
 * c23_elifdef.
 */

#ifdef A
int a;
#elifdef B
int b;
#elifndef C
int c;
#endif
