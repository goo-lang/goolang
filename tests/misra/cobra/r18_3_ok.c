/* Rule 18.3 compliant twin: < between two elements of one array. */
int f(void)
{
	int a[2] = {1, 2};
	int *p = &a[0];
	int *q = &a[1];
	if (p < q) {
		return 1;
	}
	return 0;
}
