/*
 * A bitfield missing its following comma must not merge with the next
 * declarator (`1 b` becoming `1b`).
 */

struct s {
	unsigned a:1 b:1;
};
