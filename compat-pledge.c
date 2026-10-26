#include "config.h"

extern int unused;

#ifndef HAVE_PLEDGE

#include "libks/compiler.h"

/*
 * No-op pledge(2) replacement for hosts whose libc does not provide it.  The
 * configure script defines HAVE_PLEDGE on OpenBSD; elsewhere this file lets the
 * same source compile without pretending to enforce a sandbox.
 */
int
pledge(const char *UNUSED(promises), const char *UNUSED(execpromises))
{
	return 0;
}

#endif
