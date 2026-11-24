/* Rule 18.1 compliant twin: the loop bound is reduced. */
int f(void)
{
	int a[4] = {1, 2, 3, 4};
	int *p = a;
	int s = 0;
	int i;
	for (i = 0; i <= 2; i++) {
		p = p + 1;
	}
	s = *p;
	return s;
}
