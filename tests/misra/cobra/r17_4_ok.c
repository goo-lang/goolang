/* Rule 17.4 compliant twin: an explicit return on that path. */
int f(int x)
{
	if (x > 0) {
		return 1;
	}
	x = 2;
	return x;
}
