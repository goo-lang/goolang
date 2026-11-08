/* Rule 5.3 violating control: an inner identifier hides an outer one. */
int f(int x)
{
	int count = 1;
	{
		int count = 2;
		x = x + count;
	}
	return x + count;
}
