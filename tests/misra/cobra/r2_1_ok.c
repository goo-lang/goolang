/* Rule 2.1 compliant twin: the statement moves before the return. */
int f(int x)
{
	x = 1;
	return x;
}
