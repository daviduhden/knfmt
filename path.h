/*
 * Search upward from the current directory for a file, returning an open
 * descriptor or -1. The optional out parameter receives the number of directories
 * traversed. Callers own the descriptor.
 */
int searchpath(const char *, int *);
