/* Rule 13.5 known incorrect report: strchr has no side effect, but the "-+"
 * inside its string literal reads as a pair of increment operators. */
#include <string.h>
int f(const char *s, int i)
{
	while (s[i] && strchr("-+ #0", s[i])) {
		i = i + 1;
	}
	return i;
}
