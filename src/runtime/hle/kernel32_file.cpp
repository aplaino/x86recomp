// kernel32: handles, guest<->host paths, files, directories, find.
//
// Guest paths are Windows paths. GuestGameDir() ("C:\Dungeon Siege\...")
// maps onto the game install (read-mostly); any other path maps below the
// per-user data directory ($XDG_DATA_HOME/<id>/drive_c/...), where saves and
// settings end up. Lookups are case-insensitive like NTFS.

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp {

namespace {

std::mutex g_handles_mu;
std::map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x100;

std::mutex g_cwd_mu;
std::string g_cwd;  // guest current directory; empty: GuestGameDir()

struct StdIoObject : KObject {
  explicit StdIoObject(int f) : KObject(Type::StdIo), fd(f) {}
  int fd;
};

struct FileObject : KObject {
  FileObject() : KObject(Type::File) {}
  ~FileObject() override {
    if (fd >= 0) close(fd);
  }
  int fd = -1;
};

struct FindObject : KObject {
  FindObject() : KObject(Type::Find) {}
  std::vector<std::string> names;  // matching entries (guest spelling)
  std::string host_dir;
  size_t next = 0;
};

uint32_t g_std_handles[3];

std::string UserDataRoot() { return DataDir() + "/drive_c"; }

// Splits a normalised guest path ("C:\a\b") into components after the drive.
std::vector<std::string> Components(const std::string& p) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 3; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '\\') {
      if (cur == "..") {
        if (!out.empty()) out.pop_back();
      } else if (!cur.empty() && cur != ".") {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur += p[i];
    }
  }
  return out;
}

// Absolute, normalised guest path: "C:\x\y".
std::string AbsGuest(std::string p) {
  std::replace(p.begin(), p.end(), '/', '\\');
  if (p.size() >= 2 && p[1] == ':') {
    if (p.size() == 2) p += '\\';
  } else if (!p.empty() && p[0] == '\\') {
    p = "C:" + p;
  } else {
    p = GuestCurrentDir() + "\\" + p;
  }
  std::string drive = p.substr(0, 3);
  drive[0] = char(std::toupper(drive[0]));
  std::string out = drive;
  auto comps = Components(p);
  for (size_t i = 0; i < comps.size(); i++) {
    if (i) out += '\\';
    out += comps[i];
  }
  return out;
}

// Case-insensitive resolution of path components `comps` under host dir
// `root`.
std::string Resolve(const std::string& root, const std::vector<std::string>& comps) {
  std::string cur = root;
  for (size_t i = 0; i < comps.size(); i++) {
    std::string want = comps[i];
    std::string next = cur + "/" + want;
    struct stat st;
    if (stat(next.c_str(), &st) != 0) {
      if (DIR* d = opendir(cur.c_str())) {
        std::string lw = win32::Lower(want);
        while (dirent* e = readdir(d)) {
          if (win32::Lower(e->d_name) == lw) {
            next = cur + "/" + e->d_name;
            break;
          }
        }
        closedir(d);
      }
    }
    cur = next;
  }
  return cur;
}

}  // namespace

const std::string& GuestGameDir() { return Game().guest_dir; }

std::string GuestExePath() { return GuestGameDir() + "\\" + Game().exe_name; }

std::string GuestCurrentDir() {
  std::lock_guard<std::mutex> l(g_cwd_mu);
  return g_cwd.empty() ? GuestGameDir() : g_cwd;
}

std::string HostPath(const std::string& guest_path) {
  std::string g = AbsGuest(guest_path);
  std::string lg = win32::Lower(g);
  std::string lroot = win32::Lower(GuestGameDir());
  if (lg == lroot || lg.rfind(lroot + "\\", 0) == 0) {
    auto comps = Components("C:\\" + g.substr(std::min(g.size(), lroot.size() + 1)));
    return Resolve(g_proc->game_dir, comps);
  }
  // Everything else lives in the per-user tree, by drive letter.
  std::string root = UserDataRoot();
  if (g[0] != 'C') root += std::string("/drive_") + char(std::tolower(g[0]));
  return Resolve(root, Components(g));
}

uint32_t NewHandle(std::shared_ptr<KObject> obj) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = std::move(obj);
  return h;
}

std::shared_ptr<KObject> GetHandle(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

int FileFd(KObject* o) {
  if (o->type == KObject::Type::File) return static_cast<FileObject*>(o)->fd;
  if (o->type == KObject::Type::StdIo) return static_cast<StdIoObject*>(o)->fd;
  return -1;
}

bool CloseHandleValue(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handles_mu);
  return g_handles.erase(h) != 0;
}

namespace hle {
namespace {

using namespace win32;

constexpr uint32_t kInvalidHandle = 0xFFFFFFFF;

// FILETIME from a POSIX time.
void PutFileTime(uint32_t va, const timespec& ts) {
  Wr64(va, uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100 +
               kFileTimeUnixEpoch);
}

uint32_t AttributesOf(const struct stat& st, const std::string& name) {
  uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // DIRECTORY : NORMAL
  if (!(st.st_mode & S_IWUSR)) a = (a & ~0x80u) | 0x01;  // READONLY
  if (!name.empty() && name[0] == '.' && name != "." && name != "..")
    a |= 0x02;  // HIDDEN
  return a;
}

uint32_t ErrnoToWin(int e) {
  switch (e) {
    case ENOENT: return kErrorFileNotFound;
    case ENOTDIR: return kErrorPathNotFound;
    case EACCES:
    case EPERM: return kErrorAccessDenied;
    case EEXIST: return kErrorFileExists;
    default: return kErrorAccessDenied;
  }
}

HLE("kernel32.dll", GetStdHandle, 1) {
  int32_t which = int32_t(Arg(c, 0));
  int idx = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
  if (idx < 0) return kInvalidHandle;
  if (!g_std_handles[idx])
    g_std_handles[idx] = NewHandle(std::make_shared<StdIoObject>(idx));
  return g_std_handles[idx];
}

HLE("kernel32.dll", SetStdHandle, 2) { return 1; }

HLE("kernel32.dll", GetFileType, 1) {
  auto o = GetHandle(Arg(c, 0));
  if (!o) {
    SetLastError(kErrorInvalidHandle);
    return 0;  // FILE_TYPE_UNKNOWN
  }
  return o->type == KObject::Type::StdIo ? 2 : 1;  // CHAR : DISK
}

HLE("kernel32.dll", CloseHandle, 1) {
  if (!CloseHandleValue(Arg(c, 0))) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", DuplicateHandle, 7) {
  auto o = GetHandle(Arg(c, 1));
  if (!o) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 3)) Wr32(Arg(c, 3), NewHandle(o));
  return 1;
}

HLE("kernel32.dll", CreateFileA, 7) {
  std::string gpath = GStr(Arg(c, 0));
  uint32_t access = Arg(c, 1), disp = Arg(c, 4);
  std::string path = HostPath(gpath);
  bool write = access & 0x40000000;  // GENERIC_WRITE
  bool read = access & 0x80000000;   // GENERIC_READ
  int flags = write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
  struct stat st;
  bool exists = stat(path.c_str(), &st) == 0;
  switch (disp) {
    case 1: if (exists) { SetLastError(kErrorFileExists); return kInvalidHandle; }
            flags |= O_CREAT; break;                  // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;        // CREATE_ALWAYS
    case 3: break;                                    // OPEN_EXISTING
    case 4: flags |= O_CREAT; break;                  // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;                  // TRUNCATE_EXISTING
  }
  if (exists && S_ISDIR(st.st_mode)) {
    SetLastError(kErrorAccessDenied);
    return kInvalidHandle;
  }
  if (flags & O_CREAT) {
    // Make sure the parent exists in the per-user tree.
    MakeDirs(path.substr(0, path.find_last_of('/')));
  }
  int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) {
    RECOMP_TRACE("  CreateFileA(%s) -> %s: %s\n", gpath.c_str(), path.c_str(),
              std::strerror(errno));
    SetLastError(ErrnoToWin(errno));
    return kInvalidHandle;
  }
  RECOMP_TRACE("  CreateFileA(%s) -> %s\n", gpath.c_str(), path.c_str());
  auto f = std::make_shared<FileObject>();
  f->fd = fd;
  SetLastError(exists && (disp == 2 || disp == 4) ? kErrorAlreadyExists : 0);
  return NewHandle(f);
}

int FdOf(uint32_t h) {
  auto o = GetHandle(h);
  return o ? FileFd(o.get()) : -1;
}

HLE("kernel32.dll", ReadFile, 5) {
  int fd = FdOf(Arg(c, 0));
  uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3);
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 4)) FatalF("ReadFile with OVERLAPPED is not supported");
  ssize_t r = n ? read(fd, GPtr(buf), n) : 0;
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  return 1;
}

HLE("kernel32.dll", WriteFile, 5) {
  int fd = FdOf(Arg(c, 0));
  uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3);
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  ssize_t r = n ? write(fd, GPtr(buf), n) : 0;
  if (r < 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  if (out) Wr32(out, uint32_t(r));
  return 1;
}

HLE("kernel32.dll", SetFilePointer, 4) {
  int fd = FdOf(Arg(c, 0));
  int64_t dist = int32_t(Arg(c, 1));
  uint32_t hi = Arg(c, 2), method = Arg(c, 3);
  if (hi) dist = int64_t((uint64_t(Rd32(hi)) << 32) | Arg(c, 1));
  if (fd < 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  off_t r = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) {
    SetLastError(kErrorInvalidParameter);
    return 0xFFFFFFFF;
  }
  if (hi) Wr32(hi, uint32_t(uint64_t(r) >> 32));
  SetLastError(0);
  return uint32_t(r);
}

HLE("kernel32.dll", GetFileSize, 2) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0xFFFFFFFF;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(uint64_t(st.st_size) >> 32));
  return uint32_t(st.st_size);
}

HLE("kernel32.dll", GetFileTime, 4) {
  int fd = FdOf(Arg(c, 0));
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  if (Arg(c, 1)) PutFileTime(Arg(c, 1), st.st_mtim);
  if (Arg(c, 2)) PutFileTime(Arg(c, 2), st.st_atim);
  if (Arg(c, 3)) PutFileTime(Arg(c, 3), st.st_mtim);
  return 1;
}

HLE("kernel32.dll", FlushFileBuffers, 1) {
  int fd = FdOf(Arg(c, 0));
  if (fd >= 0) fsync(fd);
  return 1;
}

HLE("kernel32.dll", GetFileAttributesA, 1) {
  std::string g = GStr(Arg(c, 0));
  std::string path = HostPath(g);
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0xFFFFFFFF;
  }
  return AttributesOf(st, path.substr(path.find_last_of('/') + 1));
}

HLE("kernel32.dll", SetFileAttributesA, 2) { return 1; }

HLE("kernel32.dll", DeleteFileA, 1) {
  if (unlink(HostPath(GStr(Arg(c, 0))).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", MoveFileA, 2) {
  std::string from = HostPath(GStr(Arg(c, 0))), to = HostPath(GStr(Arg(c, 1)));
  struct stat st;
  if (stat(to.c_str(), &st) == 0) {
    SetLastError(kErrorAlreadyExists);
    return 0;
  }
  if (rename(from.c_str(), to.c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", CreateDirectoryA, 2) {
  std::string path = HostPath(GStr(Arg(c, 0)));
  MakeDirs(path.substr(0, path.find_last_of('/')));
  if (mkdir(path.c_str(), 0755) != 0) {
    SetLastError(errno == EEXIST ? kErrorAlreadyExists : ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", RemoveDirectoryA, 1) {
  if (rmdir(HostPath(GStr(Arg(c, 0))).c_str()) != 0) {
    SetLastError(ErrnoToWin(errno));
    return 0;
  }
  return 1;
}

HLE("kernel32.dll", GetCurrentDirectoryA, 2) {
  uint32_t size = Arg(c, 0), buf = Arg(c, 1);
  std::string d = GuestCurrentDir();
  return CopyOut(buf, size, d);
}

HLE("kernel32.dll", GetFullPathNameA, 4) {
  std::string full = AbsGuest(GStr(Arg(c, 0)));
  uint32_t size = Arg(c, 1), buf = Arg(c, 2), filepart = Arg(c, 3);
  uint32_t r = CopyOut(buf, size, full);
  if (r == full.size() && filepart) {
    size_t s = full.find_last_of('\\');
    Wr32(filepart, s == std::string::npos || s + 1 == full.size()
                       ? 0
                       : buf + uint32_t(s + 1));
  }
  return r;
}

HLE("kernel32.dll", GetLongPathNameA, 3) {
  std::string full = GStr(Arg(c, 0));
  uint32_t buf = Arg(c, 1), size = Arg(c, 2);
  return CopyOut(buf, size, full);
}

HLE("kernel32.dll", GetTempPathA, 2) {
  std::string t = "C:\\TEMP\\";
  MakeDirs(UserDataRoot() + "/TEMP");
  return CopyOut(Arg(c, 1), Arg(c, 0), t);
}

HLE("kernel32.dll", GetTempFileNameA, 4) {
  static uint32_t counter = 1;
  std::string dir = GStr(Arg(c, 0)), prefix = GStr(Arg(c, 1)).substr(0, 3);
  uint32_t unique = Arg(c, 2) ? Arg(c, 2) : counter++;
  char name[32];
  std::snprintf(name, sizeof name, "%s%X.TMP", prefix.c_str(), unique & 0xFFFF);
  std::string full = dir;
  if (!full.empty() && full.back() != '\\') full += '\\';
  full += name;
  PutStr(Arg(c, 3), full);
  if (!Arg(c, 2)) {
    int fd = open(HostPath(full).c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
  }
  return unique;
}

// ---- find ------------------------------------------------------------------

void FillFindData(uint32_t fd, const std::string& host_dir, const std::string& name) {
  struct stat st{};
  stat((host_dir + "/" + name).c_str(), &st);
  std::memset(GPtr(fd), 0, 320);
  Wr32(fd + 0, AttributesOf(st, name));
  PutFileTime(fd + 4, st.st_mtim);   // creation
  PutFileTime(fd + 12, st.st_atim);  // access
  PutFileTime(fd + 20, st.st_mtim);  // write
  Wr32(fd + 28, uint32_t(uint64_t(st.st_size) >> 32));
  Wr32(fd + 32, uint32_t(st.st_size));
  PutStr(fd + 44, name.substr(0, 259));  // cFileName
  // cAlternateFileName (8.3) left empty.
}

HLE("kernel32.dll", FindFirstFileA, 2) {
  std::string pattern = AbsGuest(GStr(Arg(c, 0)));
  size_t s = pattern.find_last_of('\\');
  std::string gdir = pattern.substr(0, s), mask = pattern.substr(s + 1);
  if (gdir.size() == 2) gdir += '\\';
  std::string hdir = HostPath(gdir);
  auto f = std::make_shared<FindObject>();
  f->host_dir = hdir;
  if (DIR* d = opendir(hdir.c_str())) {
    std::string lmask = Lower(mask);
    if (lmask == "*.*") lmask = "*";
    while (dirent* e = readdir(d)) {
      if (fnmatch(lmask.c_str(), Lower(e->d_name).c_str(), 0) == 0)
        f->names.push_back(e->d_name);
    }
    closedir(d);
  }
  if (f->names.empty()) {
    SetLastError(kErrorFileNotFound);
    return kInvalidHandle;
  }
  FillFindData(Arg(c, 1), hdir, f->names[0]);
  f->next = 1;
  return NewHandle(f);
}

HLE("kernel32.dll", FindNextFileA, 2) {
  auto o = GetHandle(Arg(c, 0));
  if (!o || o->type != KObject::Type::Find) {
    SetLastError(kErrorInvalidHandle);
    return 0;
  }
  auto* f = static_cast<FindObject*>(o.get());
  if (f->next >= f->names.size()) {
    SetLastError(kErrorNoMoreFiles);
    return 0;
  }
  FillFindData(Arg(c, 1), f->host_dir, f->names[f->next++]);
  return 1;
}

HLE("kernel32.dll", FindClose, 1) {
  return CloseHandleValue(Arg(c, 0)) ? 1 : 0;
}

// ---- volumes -----------------------------------------------------------------

HLE("kernel32.dll", GetDriveTypeA, 1) { return 3; }  // DRIVE_FIXED

HLE("kernel32.dll", GetLogicalDriveStringsA, 2) {
  const char s[] = "C:\\\0";  // double-NUL terminated list
  if (Arg(c, 0) < sizeof s) return sizeof s;
  std::memcpy(GPtr(Arg(c, 1)), s, sizeof s);
  return sizeof s - 1;
}

HLE("kernel32.dll", GetVolumeInformationA, 8) {
  if (Arg(c, 1) && Arg(c, 2)) PutStr(Arg(c, 1), win32::Upper(Game().id));
  if (Arg(c, 3)) Wr32(Arg(c, 3), 0x1DEADD51);
  if (Arg(c, 4)) Wr32(Arg(c, 4), 255);
  if (Arg(c, 5)) Wr32(Arg(c, 5), 0x700FF);
  if (Arg(c, 6) && Arg(c, 7)) PutStr(Arg(c, 6), "NTFS");
  return 1;
}

HLE("kernel32.dll", GetDiskFreeSpaceExA, 4) {
  // 8 GiB free of 64 GiB, reported in 64-bit ULARGE_INTEGERs.
  if (Arg(c, 1)) Wr64(Arg(c, 1), 8ull << 30);
  if (Arg(c, 2)) Wr64(Arg(c, 2), 64ull << 30);
  if (Arg(c, 3)) Wr64(Arg(c, 3), 8ull << 30);
  return 1;
}

}  // namespace
}  // namespace hle
}  // namespace recomp
