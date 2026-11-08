/* Rule 17.4 violating control: one exit path has no return. */
int f(int x)
{
	if (x > 0) {
		return 1;
	}
	x = 2;
}
