#include <stdlib.h>
void sink(char *);
void bug(int n) {
	char *p = malloc(n);
	free(p);
	sink(p);      /* use after free */
}
