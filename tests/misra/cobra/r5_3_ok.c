/* Rule 5.3 compliant twin: the inner identifier is renamed. */
int f(int x)
{
	int count = 1;
	{
		int inner = 2;
		x = x + inner;
	}
	return x + count;
}
