/*
 * A bare `__attribute__` with no argument list must stay separated from the
 * parameter identifier (`x __attribute__` must not become `x__attribute__`).
 */

int
main(int x __attribute__)
{
}
