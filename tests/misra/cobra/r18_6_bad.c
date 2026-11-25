/* Rule 18.6 violating control: the address of an automatic object stored in a static. */
static int *keep;
void f(void)
{
	int local = 1;
	keep = &local;
}
