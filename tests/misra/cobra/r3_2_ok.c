/* Rule 3.2 compliant twin: the same comment without the splice. */
int f(int x)
{
	// this comment ends with no splice
	x = x + 1;
	return x;
}
