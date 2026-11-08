/* Rule 3.2 violating control: a line splice inside a line comment. */
int f(int x)
{
	// this comment ends in a splice \
	x = x + 1;
	return x;
}
