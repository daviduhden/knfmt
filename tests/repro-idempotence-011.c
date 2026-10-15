/*
 * A clang-format off region must never be silently dropped when a recovery
 * discards the recorded off comment. The whole source is preserved instead.
 */

static voidJfoo /*
		 * clang-format off
		 */

    // clang-format off
int x =
#ifdef FOO
	0
#else
	1
#endif
;

// clang-format on

int y;
