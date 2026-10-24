// The PRX of orbis_process_test (libTestPrx): an export the eboot calls,
// data it reads, general-dynamic TLS and an _init the loader runs before
// the eboot's entry point.

int prx_inited;
static __thread int prx_tls = 100;

int prx_add(int a, int b) { return a + b; }
int prx_tls_bump(void) { return ++prx_tls; }

void _init(void) { prx_inited = 1; }
