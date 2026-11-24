/* Rule 18.2 violating control: subtraction of pointers into different objects. */
#include <stddef.h>
ptrdiff_t f(void)
{
	int a[4] = {1, 2, 3, 4};
	int b[4] = {5, 6, 7, 8};
	int *p = &a[3];
	int *q = &b[0];
	return p - q;
}
