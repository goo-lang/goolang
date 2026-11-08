/* Rule 18.2 compliant twin: both pointers address one array. */
#include <stddef.h>
ptrdiff_t f(void)
{
	int a[4] = {1, 2, 3, 4};
	int *p = &a[3];
	int *q = &a[0];
	return p - q;
}
