/* Rule 2.1 known incorrect report: do { } while (0) is compliant. */
#define SWAP(a, b) do { int t_ = (a); (a) = (b); (b) = t_; } while (0)
void g(int *p, int *q)
{
	int a = *p;
	int b = *q;
	SWAP(a, b);
	*p = a;
	*q = b;
}
