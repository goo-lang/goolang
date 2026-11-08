/* Rule 2.1 violating control: a statement after an unconditional return. */
int f(int x)
{
	return x;
	x = 1;
}
