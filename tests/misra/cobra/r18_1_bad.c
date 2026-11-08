/* Rule 18.1 violating control: a pointer steps past one-past-the-end. */
int f(void)
{
	int a[4] = {1, 2, 3, 4};
	int *p = a;
	int s = 0;
	int i;
	for (i = 0; i <= 5; i++) {
		p = p + 1;
	}
	s = *p;
	return s;
}
