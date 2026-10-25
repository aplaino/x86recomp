// libkernel HLE: files. Guest paths are mapped by their mount point:
// /app0 (and /hostapp) is the game directory, read-only as on the PS4;
// /temp0, /download0 and the save data mounts (/savedata0..15) are
// directories below the per-user data directory. Other paths are fatal
// until something needs them.
//
// Descriptors are host file descriptors, but only those the guest opened
// are accepted. Flags, errno values and struct stat are FreeBSD's (9.x,
// which the PS4's kernel derives from).

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "orbis/hle.h"
#include "orbis/hle/kernel.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::orbis::kernel {
namespace {

// FreeBSD open(2) flags.
constexpr int kOWrOnly = 1, kORdWr = 2, kOAccMode = 3, kONonBlock = 4, kOAppend = 8,
              kOFsync = 0x80, kOCreat = 0x200, kOTrunc = 0x400, kOExcl = 0x800,
              kODirectory = 0x20000;
constexpr int kEROFS = 30;

// Host errno to FreeBSD's (the values below 35 are the same).
int FromHost(int e) {
  switch (e) {
    case EAGAIN: return kEAGAIN;
    case EDEADLK: return kEDEADLK;
    case ENAMETOOLONG: return kENAMETOOLONG;
    case ENOSYS: return kENOSYS;
    case ENOTEMPTY: return 66;
    case ELOOP: return 62;
    case ETIMEDOUT: return kETIMEDOUT;
    default: return e < 35 ? e : kEIO;
  }
}

std::mutex g_mu;
std::set<int> g_fds;  // host fds the guest opened

bool Owned(int fd) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_fds.count(fd) != 0;
}

struct Mapped {
  std::string host;
  bool read_only = false;
  bool ok = false;
};

// A guest path to a host path.
Mapped MapPath(const std::string& guest) {
  Mapped m;
  auto under = [&](const char* root) {
    const size_t n = std::strlen(root);
    return guest.compare(0, n, root) == 0 && (guest.size() == n || guest[n] == '/');
  };
  auto rest = [&](const char* root) { return guest.substr(std::strlen(root)); };
  for (const char* root : {"/app0", "/hostapp"})
    if (under(root)) {
      m.host = g_proc->game_dir + rest(root);
      m.read_only = m.ok = true;
      return m;
    }
  static const char* const kData[] = {"/temp0", "/download0"};
  for (const char* root : kData)
    if (under(root)) {
      m.host = DataDir() + root + rest(root);
      MakeDirs(DataDir() + root);
      m.ok = true;
      return m;
    }
  if (guest.compare(0, 9, "/savedata") == 0) {
    const size_t slash = guest.find('/', 1);
    const std::string mount = guest.substr(0, slash);
    m.host = DataDir() + mount + (slash == std::string::npos ? "" : guest.substr(slash));
    MakeDirs(DataDir() + mount);
    m.ok = true;
    return m;
  }
  FatalF("file path %s: no mount point maps it", guest.c_str());
}

// open(2) on the guest's terms: the fd, or -(FreeBSD errno).
int64_t Open(const std::string& path, int flags, int mode) {
  const Mapped m = MapPath(path);
  const int acc = flags & kOAccMode;
  if (m.read_only && (acc != 0 || flags & (kOCreat | kOTrunc | kOAppend))) return -kEROFS;
  int hf = acc == kORdWr ? O_RDWR : acc == kOWrOnly ? O_WRONLY : O_RDONLY;
  if (flags & kONonBlock) hf |= O_NONBLOCK;
  if (flags & kOAppend) hf |= O_APPEND;
  if (flags & kOFsync) hf |= O_SYNC;
  if (flags & kOCreat) hf |= O_CREAT;
  if (flags & kOTrunc) hf |= O_TRUNC;
  if (flags & kOExcl) hf |= O_EXCL;
  if (flags & kODirectory) hf |= O_DIRECTORY;
  const int fd = ::open(m.host.c_str(), hf | O_CLOEXEC, mode & 0777);
  if (fd < 0) return -FromHost(errno);
  std::lock_guard<std::mutex> l(g_mu);
  g_fds.insert(fd);
  return fd;
}

int64_t Close(int fd) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_fds.erase(fd)) return -kEBADF;
  }
  return ::close(fd) == 0 ? 0 : -FromHost(errno);
}

// FreeBSD 9's struct stat (120 bytes).
void PutStat(gaddr p, const struct stat& st) {
  std::memset(GPtr(p), 0, 120);
  Wr32(p + 0, uint32_t(st.st_dev));
  Wr32(p + 4, uint32_t(st.st_ino));
  Wr16(p + 8, uint16_t(st.st_mode));
  Wr16(p + 10, uint16_t(st.st_nlink));
  Wr32(p + 12, st.st_uid);
  Wr32(p + 16, st.st_gid);
  Wr32(p + 20, uint32_t(st.st_rdev));
  auto ts = [&](gaddr at, const timespec& t) {
    Wr64(at, uint64_t(t.tv_sec));
    Wr64(at + 8, uint64_t(t.tv_nsec));
  };
  ts(p + 24, st.st_atim);
  ts(p + 40, st.st_mtim);
  ts(p + 56, st.st_ctim);
  Wr64(p + 72, uint64_t(st.st_size));
  Wr64(p + 80, uint64_t(st.st_blocks));
  Wr32(p + 88, uint32_t(st.st_blksize));
  ts(p + 104, st.st_mtim);  // birth time: the host does not keep one
}

int64_t Stat(const std::string& path, gaddr out) {
  const Mapped m = MapPath(path);
  struct stat st;
  if (::stat(m.host.c_str(), &st) != 0) return -FromHost(errno);
  PutStat(out, st);
  return 0;
}

int64_t Fstat(int fd, gaddr out) {
  if (!Owned(fd)) return -kEBADF;
  struct stat st;
  if (::fstat(fd, &st) != 0) return -FromHost(errno);
  PutStat(out, st);
  return 0;
}

// read/write/pread/pwrite through a host buffer.
int64_t Read(int fd, gaddr buf, uint64_t n, int64_t off) {
  if (!Owned(fd)) return -kEBADF;
  const ssize_t r = off < 0 ? ::read(fd, GPtr(buf), n) : ::pread(fd, GPtr(buf), n, off);
  return r < 0 ? -FromHost(errno) : r;
}

int64_t Write(int fd, gaddr buf, uint64_t n, int64_t off) {
  if (fd == 1 || fd == 2) {  // the console's stdout and stderr: the host's
    const ssize_t r = ::write(fd, GPtr(buf), n);
    return r < 0 ? -FromHost(errno) : r;
  }
  if (!Owned(fd)) return -kEBADF;
  const ssize_t r = off < 0 ? ::write(fd, GPtr(buf), n) : ::pwrite(fd, GPtr(buf), n, off);
  return r < 0 ? -FromHost(errno) : r;
}

int64_t Lseek(int fd, int64_t off, int whence) {
  if (!Owned(fd)) return -kEBADF;
  if (whence < 0 || whence > 2) return -kEINVAL;  // SEEK_SET, CUR, END as on the host
  const off_t r = ::lseek(fd, off, whence);
  return r < 0 ? -FromHost(errno) : r;
}

int64_t PathOp(const std::string& path, int (*op)(const char*)) {
  const Mapped m = MapPath(path);
  if (m.read_only) return -kEROFS;
  return op(m.host.c_str()) == 0 ? 0 : -FromHost(errno);
}

// The SCE functions return the error as a code, the POSIX ones set errno.
uint64_t Sce(int64_t r) { return r < 0 ? SceError(int(-r)) : uint64_t(r); }
uint64_t Posix(const Ctx& c, int64_t r) {
  if (r >= 0) return uint64_t(r);
  SetErrno(c, int(-r));
  return ~uint64_t(0);
}

}  // namespace
}  // namespace recomp::orbis::kernel

using namespace recomp::orbis::kernel;
using recomp::GStr;
using recomp::hle::Arg;

// int sceKernelOpen(const char* path, int flags, SceKernelMode mode)
ORBIS_HLE("libkernel", sceKernelOpen, 3) {
  return Sce(Open(GStr(Arg(c, 0)), int(Arg(c, 1)), int(Arg(c, 2))));
}
ORBIS_HLE("libkernel", open, 3) {
  return Posix(c, Open(GStr(Arg(c, 0)), int(Arg(c, 1)), int(Arg(c, 2))));
}
ORBIS_HLE("libkernel", sceKernelClose, 1) { return Sce(Close(int(Arg(c, 0)))); }
ORBIS_HLE("libkernel", close, 1) { return Posix(c, Close(int(Arg(c, 0)))); }

ORBIS_HLE("libkernel", sceKernelRead, 3) {
  return Sce(Read(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1));
}
ORBIS_HLE("libkernel", _read, 3) { return Posix(c, Read(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1)); }
ORBIS_HLE("libkernel", read, 3) { return Posix(c, Read(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1)); }
ORBIS_HLE("libkernel", sceKernelPread, 4) {
  return Sce(Read(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), int64_t(Arg(c, 3))));
}
ORBIS_HLE("libkernel", sceKernelWrite, 3) {
  return Sce(Write(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1));
}
ORBIS_HLE("libkernel", _write, 3) {
  return Posix(c, Write(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1));
}
ORBIS_HLE("libkernel", write, 3) {
  return Posix(c, Write(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), -1));
}
ORBIS_HLE("libkernel", sceKernelPwrite, 4) {
  return Sce(Write(int(Arg(c, 0)), Arg(c, 1), Arg(c, 2), int64_t(Arg(c, 3))));
}
ORBIS_HLE("libkernel", sceKernelLseek, 3) {
  return Sce(Lseek(int(Arg(c, 0)), int64_t(Arg(c, 1)), int(Arg(c, 2))));
}
ORBIS_HLE("libkernel", lseek, 3) {
  return Posix(c, Lseek(int(Arg(c, 0)), int64_t(Arg(c, 1)), int(Arg(c, 2))));
}

ORBIS_HLE("libkernel", sceKernelStat, 2) { return Sce(Stat(GStr(Arg(c, 0)), Arg(c, 1))); }
ORBIS_HLE("libkernel", stat, 2) { return Posix(c, Stat(GStr(Arg(c, 0)), Arg(c, 1))); }
ORBIS_HLE("libkernel", sceKernelFstat, 2) { return Sce(Fstat(int(Arg(c, 0)), Arg(c, 1))); }
ORBIS_HLE("libkernel", fstat, 2) { return Posix(c, Fstat(int(Arg(c, 0)), Arg(c, 1))); }

ORBIS_HLE("libkernel", sceKernelMkdir, 2) {
  const Mapped m = MapPath(GStr(Arg(c, 0)));
  if (m.read_only) return SceError(kEROFS);
  return ::mkdir(m.host.c_str(), mode_t(Arg(c, 1) & 0777)) == 0 ? 0 : SceError(FromHost(errno));
}
ORBIS_HLE("libkernel", sceKernelUnlink, 1) { return Sce(PathOp(GStr(Arg(c, 0)), ::unlink)); }
ORBIS_HLE("libkernel", sceKernelRmdir, 1) { return Sce(PathOp(GStr(Arg(c, 0)), ::rmdir)); }
ORBIS_HLE("libkernel", sceKernelRename, 2) {
  const Mapped from = MapPath(GStr(Arg(c, 0))), to = MapPath(GStr(Arg(c, 1)));
  if (from.read_only || to.read_only) return SceError(kEROFS);
  return ::rename(from.host.c_str(), to.host.c_str()) == 0 ? 0 : SceError(FromHost(errno));
}
ORBIS_HLE("libkernel", sceKernelFsync, 1) {
  if (!Owned(int(Arg(c, 0)))) return SceError(kEBADF);
  return ::fsync(int(Arg(c, 0))) == 0 ? 0 : SceError(FromHost(errno));
}
ORBIS_HLE("libkernel", sceKernelFtruncate, 2) {
  if (!Owned(int(Arg(c, 0)))) return SceError(kEBADF);
  return ::ftruncate(int(Arg(c, 0)), off_t(Arg(c, 1))) == 0 ? 0 : SceError(FromHost(errno));
}
ORBIS_HLE("libkernel", sceKernelTruncate, 2) {
  const Mapped m = MapPath(GStr(Arg(c, 0)));
  if (m.read_only) return SceError(kEROFS);
  return ::truncate(m.host.c_str(), off_t(Arg(c, 1))) == 0 ? 0 : SceError(FromHost(errno));
}
