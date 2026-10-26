#ifndef ARENAS_H
#define ARENAS_H

/*
 * The five process-wide arenas. eternal holds objects that must survive every
 * file (style, shared tables, token strings); scratch is reset by its users per
 * operation; doc holds document trees; buffer backs temporary rendering buffers;
 * ruler holds alignment state. Initialize once and free once.
 */
struct arenas {
	struct arena *eternal;
	struct arena *scratch;
	struct arena *doc;
	struct arena *buffer;
	struct arena *ruler;
};

void arenas_init(struct arenas *);
void arenas_free(struct arenas *);

#endif /* !ARENAS_H */
