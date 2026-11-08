/* Rule 18.6 compliant twin: the address goes to an automatic of the same scope. */
void f(void)
{
	int local = 1;
	int *keep = &local;
	*keep = 2;
}
