#include <stdlib.h>
void ok(int n) {
	char *p = malloc(n);
	if (!p) return;
	p[0] = 1;
	free(p);
}
