#include <err.h>
#include <limits.h>
#include <string.h>

#include "config.h"
#include "options.h"

static int ctotrace(char c);

/*
 * Reset all options to their defaults: no mode is selected and every trace
 * class is disabled. The bitfields are therefore false and the trace array is
 * zeroed.
 */
void
options_init(struct options *op)
{
	memset(op, 0, sizeof(*op));
}

/*
 * Parse a string of trace flag characters, as accepted by -t. Each character
 * enables one trace class and may be repeated to increase its verbosity. The
 * special 'a' flag (TRACE_ALL, index zero) enables every class at maximum
 * verbosity. An unknown character is diagnosed and makes the caller exit.
 */
int
options_trace_parse(struct options *op, const char *flags)
{
	for (; *flags != '\0'; flags++) {
		int idx;

		idx = ctotrace(*flags);
		if (idx == -1) {
			warnx("%c: unknown trace flag", *flags);
			return 1;
		} else if (idx == 0) {
			size_t i;

			for (i = 0; i < TRACE_MAX; i++)
				op->trace[i] = UINT_MAX;
		} else {
			op->trace[idx]++;
		}
	}
	return 0;
}

/*
 * Map a trace flag character to a trace class index. Returns -1 for an unknown
 * character; zero denotes TRACE_ALL and is handled specially by the caller.
 */
static int
ctotrace(char c)
{
	switch (c) {
#define OP(name, shortname)                                                    \
	case shortname:                                                        \
		return TRACE_##name;
		FOR_TRACE_TYPES(OP)
#undef OP
	}
	return -1;
}
