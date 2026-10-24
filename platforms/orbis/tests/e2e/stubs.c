// Link-time stand-ins that give the test programs' imports a library
// (tools/orbis_sce_elf.py --lib): compiled once per library, with
// -DLIB_TEST or -DLIB_KERNEL. Never run.

#ifdef LIB_TEST
int test_check(int id, long got, long want) { return 0; }
int test_double(int id, double d, long n) { return 0; }
int test_value;
#endif

#ifdef LIB_KERNEL
void* __tls_get_addr(void* ti) { return 0; }
#endif
