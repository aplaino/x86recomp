// The PRX orbis_process_test's eboot loads itself (libTestLoad): no
// module needs it at start, so the loader maps it when the eboot asks
// (sceKernelLoadStartModule) and finds load_mul in it (sceKernelDlsym).

static long started;

int load_mul(int a, int b) { return a * b + (int)started; }

// module_start's arguments, passed through: the size of the argument
// block and the block.
int _init(unsigned long args, const int* argp) {
  started = (long)args;
  return (int)args + *argp;
}
