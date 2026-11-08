/* Rule 2.6 violating control: a label that no goto targets. */
int f(int x)
{
unused:
	return x + 1;
}
