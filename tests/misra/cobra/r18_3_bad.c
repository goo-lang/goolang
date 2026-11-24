/* Rule 18.3 violating control: < between addresses of unrelated objects. */
int f(void)
{
	int a = 1;
	int b = 2;
	int *p = &a;
	int *q = &b;
	if (p < q) {
		return 1;
	}
	return 0;
}
