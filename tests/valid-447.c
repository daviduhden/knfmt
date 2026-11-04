/*
 * c99_designated.
 */

struct s {
	int a;
	int b;
};
struct s x = { .a = 1, .b = 2 };
int y[8] = { [3] = 1, [5] = 2 };
