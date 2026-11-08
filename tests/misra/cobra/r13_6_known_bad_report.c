/* Rule 13.6 known incorrect report: sizeof with no parentheses. */
int f(int x)
{
	int n = sizeof x;
	return n;
}
