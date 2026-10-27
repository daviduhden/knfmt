#include <stdarg.h>
#include <stdio.h>

#include "config.h"
#include "trace.h"

static char
trace_type_str(enum trace_type type)
{
	switch (type) {
#define OP(name, shortname)                                                                        \
	case TRACE_##name:                                                                         \
		return shortname;
		FOR_TRACE_TYPES(OP)
#undef OP
	case TRACE_MAX:
		break;
	}
	return '-';
}

/*
 * Emit one trace line prefixed with the trace class and, when known, the
 * calling function. Tracing is diagnostic only and writes to standard error.
 */
void
trace_impl(enum trace_type type, const char *fun, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "[%c]", trace_type_str(type));
	if (fun != nullptr)
		fprintf(stderr, " %s:", fun);
	fprintf(stderr, " ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}
