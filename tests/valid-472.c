/*
 * c23_attrs.
 */

[[nodiscard]] int f(void);
static [[maybe_unused]] int x;
int y [[deprecated]];
int f(void) [[nodiscard]];
