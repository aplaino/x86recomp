// orbis_process_test's checks of the libkernel HLE (orbis/hle/kernel_*):
// threads, mutexes, condition variables, semaphores, event flags, once,
// thread-specific data, errno, memory, clocks and files, from lifted
// code. kernel_checks() returns how many failed; each reports to the
// test's HLE (test_check).

int test_check(int id, long got, long want);

typedef void* Handle;  // ScePthread, ScePthreadMutex, SceKernelSema, ...
typedef struct {
  int state;
  void* mutex;
} Once;

int scePthreadCreate(Handle* t, const Handle* attr, void* (*entry)(void*), void* arg,
                     const char* name);
int scePthreadJoin(Handle t, void** ret);
int scePthreadMutexattrInit(Handle* a);
int scePthreadMutexattrSettype(Handle* a, int type);
int scePthreadMutexattrDestroy(Handle* a);
int scePthreadMutexInit(Handle* m, const Handle* attr, const char* name);
int scePthreadMutexLock(Handle* m);
int scePthreadMutexTrylock(Handle* m);
int scePthreadMutexUnlock(Handle* m);
int scePthreadMutexDestroy(Handle* m);
int scePthreadCondInit(Handle* cv, const Handle* attr, const char* name);
int scePthreadCondWait(Handle* cv, Handle* m);
int scePthreadCondSignal(Handle* cv);
int scePthreadCondBroadcast(Handle* cv);
int scePthreadKeyCreate(int* key, void (*dtor)(void*));
int scePthreadSetspecific(int key, const void* value);
void* scePthreadGetspecific(int key);
int scePthreadOnce(Once* once, void (*init)(void));
int sceKernelCreateSema(Handle* s, const char* name, unsigned attr, int init, int max,
                        const void* opt);
int sceKernelWaitSema(Handle s, int need, unsigned* timeout_us);
int sceKernelSignalSema(Handle s, int n);
int sceKernelCreateEventFlag(Handle* f, const char* name, unsigned attr, unsigned long init,
                             const void* opt);
int sceKernelSetEventFlag(Handle f, unsigned long bits);
int sceKernelWaitEventFlag(Handle f, unsigned long pattern, unsigned mode,
                           unsigned long* result, unsigned* timeout_us);
int* __error(void);
int open(const char* path, int flags, int mode);
int sceKernelOpen(const char* path, int flags, int mode);
long sceKernelRead(int fd, void* buf, unsigned long n);
int sceKernelClose(int fd);
int sceKernelStat(const char* path, void* st);
void* mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off);
int munmap(void* addr, unsigned long len);
int sceKernelUsleep(unsigned us);
unsigned long sceKernelGetProcessTime(void);
int clock_gettime(int clock, long ts[2]);

#define SCE_EDEADLK 0x8002000B
#define SCE_EBUSY 0x80020010
#define SCE_ETIMEDOUT 0x8002003C

static __thread int tls_value = 5;

// Shared state of the threads below.
static Handle counter_mu;
static long counter;
static Handle cv, cv_mu;
static int turn, pongs;
static Handle sema;
static Handle flag;
static int dtor_runs, once_runs;
static Once once;

static void* count_up(void* arg) {
  for (int i = 0; i < 20000; i++) {
    scePthreadMutexLock(&counter_mu);
    counter++;
    scePthreadMutexUnlock(&counter_mu);
  }
  return (void*)((long)arg + 100);
}

static void* tls_and_try(void* arg) {
  (void)arg;
  long r = tls_value == 5 ? 1 : 0;  // a fresh copy of the template
  if (scePthreadMutexTrylock(&counter_mu) == (int)SCE_EBUSY) r |= 2;  // main holds it
  return (void*)r;
}

static void* pong(void* arg) {
  (void)arg;
  for (int i = 0; i < 1000; i++) {
    scePthreadMutexLock(&cv_mu);
    while (turn != 1) scePthreadCondWait(&cv, &cv_mu);
    turn = 0;
    pongs++;
    scePthreadCondSignal(&cv);
    scePthreadMutexUnlock(&cv_mu);
  }
  return 0;
}

static void* signaller(void* arg) {
  (void)arg;
  sceKernelUsleep(2000);
  sceKernelSignalSema(sema, 2);
  sceKernelSetEventFlag(flag, 0x5);
  return 0;
}

// Woken together by a broadcast, each must get the mutex back alone.
static int go, waiting, inside, overlap;
static void* wait_go(void* arg) {
  (void)arg;
  scePthreadMutexLock(&cv_mu);
  waiting++;
  while (!go) scePthreadCondWait(&cv, &cv_mu);
  if (++inside > 1) overlap = 1;
  sceKernelUsleep(500);
  inside--;
  scePthreadMutexUnlock(&cv_mu);
  return 0;
}

static void dtor(void* v) { dtor_runs += (int)(long)v; }
static void* with_specific(void* arg) {
  int key = (int)(long)arg;
  scePthreadSetspecific(key, (void*)7);
  return scePthreadGetspecific(key);
}

static void once_init(void) { once_runs++; }
static void* call_once(void* arg) {
  (void)arg;
  scePthreadOnce(&once, once_init);
  return 0;
}

int kernel_checks(void) {
  int failed = 0;
  Handle t[4];
  void* ret;

  // 40-43: a mutex-protected counter from four threads, and their
  // results through join.
  scePthreadMutexInit(&counter_mu, 0, "counter");
  for (long i = 0; i < 4; i++) scePthreadCreate(&t[i], 0, count_up, (void*)i, "count");
  long rets = 0;
  for (int i = 0; i < 4; i++) {
    scePthreadJoin(t[i], &ret);
    rets += (long)ret;
  }
  failed += test_check(40, counter, 80000);
  failed += test_check(41, rets, 406);

  // Error-checking (the default) and recursive mutexes.
  scePthreadMutexLock(&counter_mu);
  failed += test_check(42, (unsigned)scePthreadMutexLock(&counter_mu), SCE_EDEADLK);
  scePthreadCreate(&t[0], 0, tls_and_try, 0, "try");
  scePthreadJoin(t[0], &ret);
  failed += test_check(43, (long)ret, 3);  // template TLS, EBUSY
  scePthreadMutexUnlock(&counter_mu);
  Handle attr, rec;
  scePthreadMutexattrInit(&attr);
  scePthreadMutexattrSettype(&attr, 2);
  scePthreadMutexInit(&rec, &attr, "recursive");
  scePthreadMutexattrDestroy(&attr);
  int r1 = scePthreadMutexLock(&rec), r2 = scePthreadMutexLock(&rec);
  int r3 = scePthreadMutexUnlock(&rec), r4 = scePthreadMutexUnlock(&rec);
  failed += test_check(44, r1 | r2 | r3 | r4, 0);
  failed += test_check(45, scePthreadMutexDestroy(&rec), 0);

  // 46: a condition variable ping-pong.
  scePthreadMutexInit(&cv_mu, 0, "cv");
  scePthreadCondInit(&cv, 0, "cv");
  scePthreadCreate(&t[0], 0, pong, 0, "pong");
  for (int i = 0; i < 1000; i++) {
    scePthreadMutexLock(&cv_mu);
    while (turn != 0) scePthreadCondWait(&cv, &cv_mu);
    turn = 1;
    scePthreadCondSignal(&cv);
    scePthreadMutexUnlock(&cv_mu);
  }
  scePthreadJoin(t[0], 0);
  failed += test_check(46, pongs, 1000);
  for (int i = 0; i < 4; i++) scePthreadCreate(&t[i], 0, wait_go, 0, "wait");
  for (int ready = 0; ready < 4;) {
    sceKernelUsleep(200);
    scePthreadMutexLock(&cv_mu);
    ready = waiting;
    scePthreadMutexUnlock(&cv_mu);
  }
  scePthreadMutexLock(&cv_mu);
  go = 1;
  scePthreadCondBroadcast(&cv);
  scePthreadMutexUnlock(&cv_mu);
  for (int i = 0; i < 4; i++) scePthreadJoin(t[i], 0);
  failed += test_check(62, overlap, 0);

  // 47-50: a semaphore and an event flag set by another thread, and a
  // semaphore wait that times out.
  sceKernelCreateSema(&sema, "sema", 1, 0, 4, 0);
  sceKernelCreateEventFlag(&flag, "flag", 0x20, 0, 0);
  unsigned timeout = 1000;
  failed += test_check(47, (unsigned)sceKernelWaitSema(sema, 1, &timeout), SCE_ETIMEDOUT);
  scePthreadCreate(&t[0], 0, signaller, 0, "signal");
  failed += test_check(48, sceKernelWaitSema(sema, 2, 0), 0);
  unsigned long bits = 0;
  failed += test_check(49, sceKernelWaitEventFlag(flag, 0x5, 1 | 0x20, &bits, 0), 0);
  failed += test_check(50, (long)bits, 5);
  scePthreadJoin(t[0], 0);

  // 51-52: thread-specific data and its destructor; once.
  int key;
  scePthreadKeyCreate(&key, dtor);
  scePthreadCreate(&t[0], 0, with_specific, (void*)(long)key, "specific");
  scePthreadJoin(t[0], &ret);
  failed += test_check(51, (long)ret * 10 + dtor_runs, 77);
  for (int i = 0; i < 4; i++) scePthreadCreate(&t[i], 0, call_once, 0, "once");
  for (int i = 0; i < 4; i++) scePthreadJoin(t[i], 0);
  failed += test_check(52, once_runs, 1);

  // 53-54: errno, and an SCE error code, for a missing file.
  failed += test_check(53, open("/app0/missing", 0, 0), -1);
  failed += test_check(54, *__error(), 2);  // ENOENT

  // 55-57: a file in /app0 (the test's game directory: "hello\n").
  char buf[16] = {0};
  int fd = sceKernelOpen("/app0/data.txt", 0, 0);
  failed += test_check(55, sceKernelRead(fd, buf, sizeof buf), 6);
  failed += test_check(56, buf[0] == 'h' && buf[4] == 'o', 1);
  sceKernelClose(fd);
  long st[15];
  failed += test_check(57, sceKernelStat("/app0/data.txt", st) == 0 ? st[9] : -1, 6);  // st_size
  failed += test_check(58, (unsigned)sceKernelOpen("/app0/new.txt", 0x201, 0644),
                       0x8002001E);  // EROFS: app0 is read-only

  // 59: anonymous memory.
  long* p = (long*)mmap(0, 0x10000, 3, 0x1000 | 2, -1, 0);
  p[0] = 42;
  p[0x1FFF] = 43;
  failed += test_check(59, p[0] + p[0x1FFF] + munmap(p, 0x10000), 85);

  // 60-61: clocks.
  long a[2], b[2];
  unsigned long t0 = sceKernelGetProcessTime();
  clock_gettime(4, a);  // CLOCK_MONOTONIC
  sceKernelUsleep(3000);
  clock_gettime(4, b);
  failed += test_check(60, sceKernelGetProcessTime() - t0 >= 3000, 1);
  failed += test_check(61, (b[0] - a[0]) * 1000000000 + (b[1] - a[1]) >= 3000000, 1);
  return failed;
}
