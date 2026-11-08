/* Rule 18.6 known incorrect report: the buffer is static, so its address may
 * persist after the function returns. */
char *get(void)
{
	static char buf[16];
	return buf;
}
