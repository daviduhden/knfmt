/*
 * Conditional compilation must be preserved by every branch form: #if/#else,
 * #ifdef/#elif/#else, #ifndef/#elifdef/#elifndef, and conditionals that cross
 * a function parameter list.
 */

#if 1
int a;
#else
int b;
#endif

#ifdef A
int c;
#elif defined(B)
int d;
#else
int e;
#endif

#ifndef C
int f;
#elifdef D
int g;
#elifndef E
int h;
#endif

int
conditional_args(
#ifdef A
    int x
#else
    int y
#endif
    ,
    int z)
{
	return 0;
}
