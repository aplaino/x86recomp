// xboxkrnl: files and directories (Nt*File), symbolic links.
//
// A file handle names a kernel object in guest memory (a notification
// event's header, signalled when an I/O on it completes, so a wait on the
// handle sees the completion) with the host file behind it. I/O is
// carried out at once: an asynchronous request completes before it
// returns, with its event set and its IO_STATUS_BLOCK written, which
// callers that wait for STATUS_PENDING handle as a completed request.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ppc/fatal.h"
#include "xbox360/fs.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

constexpr const char* kKrnl = "xboxkrnl.exe";

// Access rights that write.
constexpr uint32_t kWriteAccess = 0x40000000 /* GENERIC_WRITE */ | 0x10000000 /* GENERIC_ALL */ |
                                  0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */ |
                                  0x10000 /* DELETE */;
// Create dispositions and options.
enum : uint32_t { kSupersede, kOpen, kCreate, kOpenIf, kOverwrite, kOverwriteIf };
constexpr uint32_t kDirectoryFile = 0x1, kSynchronousAlert = 0x10, kSynchronousNonAlert = 0x20,
                   kNonDirectoryFile = 0x40, kDeleteOnClose = 0x1000;
// IO_STATUS_BLOCK.Information of a create.
enum : uint32_t { kSuperseded, kOpened, kCreated, kOverwritten };
// Information classes.
enum : uint32_t {
  kFileBasic = 4, kFileStandard = 5, kFileInternal = 6, kFileRename = 10, kFileDisposition = 13,
  kFilePosition = 14, kFileAllocation = 19, kFileEndOfFile = 20, kFileNetworkOpen = 34,
  kFsVolume = 1, kFsSize = 3, kFsDevice = 4, kFsAttribute = 5,
};

struct File {
  Location loc;
  int fd = -1;  // -1: a directory
  bool synchronous = false, delete_on_close = false;
  uint64_t position = 0;
  // A directory's listing, taken at its first query (or a restart).
  std::vector<std::string> entries;
  size_t next = 0;
  bool listed = false;
  std::mutex lock;
};

std::mutex g_files_lock;
std::unordered_map<uint32_t, File*> g_files;  // kernel object -> file

void DestroyFile(uint32_t obj) {
  File* f;
  {
    std::lock_guard<std::mutex> g(g_files_lock);
    f = g_files.at(obj);
    g_files.erase(obj);
  }
  if (f->fd >= 0) close(f->fd);
  if (f->delete_on_close) {
    std::error_code ec;
    std::filesystem::remove(f->loc.host, ec);
  }
  delete f;
  SystemFree(obj);
}

File* FileOf(uint32_t handle) {
  uint32_t obj = HandleObject(handle);
  std::lock_guard<std::mutex> g(g_files_lock);
  auto it = g_files.find(obj);
  return it == g_files.end() ? nullptr : it->second;
}

uint32_t NewFileHandle(File* f) {
  uint32_t obj = SystemAlloc(kDispatcherHeaderSize);
  InitObject(obj, kNotificationEvent, 1);
  {
    std::lock_guard<std::mutex> g(g_files_lock);
    g_files[obj] = f;
  }
  Own(obj, DestroyFile);
  return NewHandle(obj);
}

void SetIoStatus(uint32_t io_status, uint32_t status, uint32_t information) {
  if (!io_status) return;
  Wr32(io_status, status);
  Wr32(io_status + 4, information);
}

// The end of an I/O: its status block, its event and the file object
// signalled.
uint32_t Complete(uint32_t handle, uint32_t event, uint32_t io_status, uint32_t status, uint32_t info) {
  SetIoStatus(io_status, status, info);
  if (uint32_t e = event ? HandleObject(event) : 0) SetSignal(e, 1);
  SetSignal(HandleObject(handle), 1);
  return status;
}

// The path OBJECT_ATTRIBUTES name (RootDirectory, ObjectName, Attributes),
// relative to the root directory's handle if it has one.
uint32_t PathOf(uint32_t attributes, Location& loc) {
  std::string name = ReadAnsiString(Rd32(attributes + 4));
  if (uint32_t root = Rd32(attributes)) {
    File* dir = FileOf(root);
    if (!dir) return kStatusInvalidHandle;
    name = dir->loc.guest + "\\" + name;
  }
  return Resolve(name, loc);
}

void RefuseApc(ppc::Ctx& c, uint32_t apc, const char* what) {
  if (apc) ppc::Fatal(&c, "%s with an APC routine: APCs are not supported yet", what);
}

// The guest's byte offset of an I/O: the file's position when there is
// none or it is FILE_USE_FILE_POINTER_POSITION (-2, high -1).
uint64_t Offset(const File& f, uint32_t ptr) {
  if (!ptr) return f.position;
  uint64_t v = Rd64(ptr);
  return v == 0xFFFFFFFFFFFFFFFEull ? f.position : v;
}

uint64_t Allocation(uint64_t size, const Device& d) { return (size + d.cluster - 1) / d.cluster * d.cluster; }

// FILE_NETWORK_OPEN_INFORMATION: the times, allocation size, size and
// attributes.
void PutNetworkOpen(uint32_t info, const struct stat& st, const Device& d) {
  uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
  Wr64(info + 0x00, ModifyTime(st));
  Wr64(info + 0x08, AccessTime(st));
  Wr64(info + 0x10, ModifyTime(st));
  Wr64(info + 0x18, ModifyTime(st));
  Wr64(info + 0x20, Allocation(size, d));
  Wr64(info + 0x28, size);
  Wr32(info + 0x30, FileAttributes(st, d));
  Wr32(info + 0x34, 0);
}

uint32_t Open(uint32_t handle_ptr, uint32_t access, uint32_t attributes, uint32_t io_status,
              uint32_t disposition, uint32_t options) {
  Location loc;
  uint32_t status = PathOf(attributes, loc);
  if (status != kStatusSuccess) {
    SetIoStatus(io_status, status, 0);
    return status;
  }
  struct stat st;
  bool exists = stat(loc.host.c_str(), &st) == 0;
  bool is_dir = exists && S_ISDIR(st.st_mode);
  auto fail = [&](uint32_t s) {
    SetIoStatus(io_status, s, 0);
    return s;
  };
  if (disposition > kOverwriteIf) return fail(kStatusInvalidParameter);
  if (!exists && (disposition == kOpen || disposition == kOverwrite)) return fail(kStatusObjectNameNotFound);
  if (exists && disposition == kCreate) return fail(kStatusObjectNameCollision);
  if (is_dir && (options & kNonDirectoryFile)) return fail(kStatusFileIsADirectory);
  if (exists && !is_dir && (options & kDirectoryFile)) return fail(kStatusNotADirectory);
  bool create = !exists, truncate = exists && !is_dir && disposition != kOpen && disposition != kOpenIf &&
                                    disposition != kCreate;
  if (!loc.device->writable && ((access & kWriteAccess) || create || truncate))
    return fail(kStatusAccessDenied);

  auto* f = new File;
  f->loc = loc;
  f->synchronous = options & (kSynchronousAlert | kSynchronousNonAlert);
  f->delete_on_close = options & kDeleteOnClose;
  if (create && (options & kDirectoryFile)) {
    if (mkdir(loc.host.c_str(), 0755) != 0) {
      delete f;
      return fail(StatusFromErrno(errno));
    }
  } else if (!is_dir) {
    int flags = (access & kWriteAccess) && loc.device->writable ? O_RDWR : O_RDONLY;
    if (create) flags |= O_CREAT | O_EXCL;
    if (truncate) flags |= O_TRUNC;
    f->fd = open(loc.host.c_str(), flags | O_CLOEXEC, 0644);
    if (f->fd < 0) {
      delete f;
      return fail(StatusFromErrno(errno));
    }
  }
  Wr32(handle_ptr, NewFileHandle(f));
  SetIoStatus(io_status, kStatusSuccess,
              create ? kCreated : !truncate ? kOpened : disposition == kSupersede ? kSuperseded : kOverwritten);
  return kStatusSuccess;
}

}  // namespace

// NtCreateFile(*handle, access, attributes, *io_status, *allocation size,
// file attributes, share access, disposition, options).
HLE(kKrnl, 210, NtCreateFile) {
  return Open(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 7), Arg(c, 8));
}

// NtOpenFile(*handle, access, attributes, *io_status, share access, options).
HLE(kKrnl, 223, NtOpenFile) { return Open(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), kOpen, Arg(c, 5)); }

// NtReadFile(handle, event, APC routine, APC context, *io_status, buffer,
// length, *byte offset).
HLE(kKrnl, 240, NtReadFile) {
  uint32_t handle = Arg(c, 0), event = Arg(c, 1), io_status = Arg(c, 4), buf = Arg(c, 5), len = Arg(c, 6);
  RefuseApc(c, Arg(c, 2), "NtReadFile");
  File* f = FileOf(handle);
  if (!f) return kStatusInvalidHandle;
  if (f->fd < 0) return Complete(handle, event, io_status, kStatusFileIsADirectory, 0);
  std::lock_guard<std::mutex> g(f->lock);
  uint64_t off = Offset(*f, Arg(c, 7));
  ssize_t n = pread(f->fd, Mem() + buf, len, off_t(off));
  if (n < 0) return Complete(handle, event, io_status, StatusFromErrno(errno), 0);
  f->position = off + uint64_t(n);
  return Complete(handle, event, io_status, n == 0 && len ? kStatusEndOfFile : kStatusSuccess, uint32_t(n));
}

// NtWriteFile(handle, event, APC routine, APC context, *io_status, buffer,
// length, *byte offset): FILE_WRITE_TO_END_OF_FILE (-1) appends.
HLE(kKrnl, 255, NtWriteFile) {
  uint32_t handle = Arg(c, 0), event = Arg(c, 1), io_status = Arg(c, 4), buf = Arg(c, 5), len = Arg(c, 6);
  RefuseApc(c, Arg(c, 2), "NtWriteFile");
  File* f = FileOf(handle);
  if (!f) return kStatusInvalidHandle;
  if (f->fd < 0) return Complete(handle, event, io_status, kStatusFileIsADirectory, 0);
  if (!f->loc.device->writable) return Complete(handle, event, io_status, kStatusAccessDenied, 0);
  std::lock_guard<std::mutex> g(f->lock);
  uint64_t off = Offset(*f, Arg(c, 7));
  if (off == ~0ull) {
    struct stat st;
    fstat(f->fd, &st);
    off = uint64_t(st.st_size);
  }
  ssize_t n = pwrite(f->fd, Mem() + buf, len, off_t(off));
  if (n < 0) return Complete(handle, event, io_status, StatusFromErrno(errno), 0);
  f->position = off + uint64_t(n);
  return Complete(handle, event, io_status, kStatusSuccess, uint32_t(n));
}

// NtQueryInformationFile(handle, *io_status, info, length, class).
HLE(kKrnl, 232, NtQueryInformationFile) {
  uint32_t io_status = Arg(c, 1), info = Arg(c, 2), len = Arg(c, 3), cls = Arg(c, 4);
  File* f = FileOf(Arg(c, 0));
  if (!f) return kStatusInvalidHandle;
  struct stat st;
  if (stat(f->loc.host.c_str(), &st) != 0) {
    SetIoStatus(io_status, StatusFromErrno(errno), 0);
    return StatusFromErrno(errno);
  }
  const Device& d = *f->loc.device;
  uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
  uint32_t need;
  switch (cls) {
    case kFileBasic: need = 0x28; break;
    case kFileStandard: need = 0x18; break;
    case kFileInternal: case kFilePosition: need = 8; break;
    case kFileNetworkOpen: need = 0x38; break;
    default: ppc::Fatal(&c, "NtQueryInformationFile: class %u is not supported yet", cls);
  }
  if (len < need) {
    SetIoStatus(io_status, kStatusInfoLengthMismatch, 0);
    return kStatusInfoLengthMismatch;
  }
  switch (cls) {
    case kFileBasic:  // the times, then the attributes as NT has them (a console check is pending)
      Wr64(info + 0x00, ModifyTime(st));
      Wr64(info + 0x08, AccessTime(st));
      Wr64(info + 0x10, ModifyTime(st));
      Wr64(info + 0x18, ModifyTime(st));
      Wr32(info + 0x20, FileAttributes(st, d));
      Wr32(info + 0x24, 0);
      break;
    case kFileStandard:  // allocation size, size, links, delete pending, directory
      Wr64(info + 0x00, Allocation(size, d));
      Wr64(info + 0x08, size);
      Wr32(info + 0x10, 1);
      Wr8(info + 0x14, f->delete_on_close);
      Wr8(info + 0x15, S_ISDIR(st.st_mode));
      Wr16(info + 0x16, 0);
      break;
    case kFileInternal: Wr64(info, uint64_t(st.st_ino)); break;
    case kFilePosition: Wr64(info, f->position); break;
    case kFileNetworkOpen: PutNetworkOpen(info, st, d); break;
  }
  SetIoStatus(io_status, kStatusSuccess, need);
  return kStatusSuccess;
}

// NtSetInformationFile(handle, *io_status, info, length, class).
HLE(kKrnl, 247, NtSetInformationFile) {
  uint32_t io_status = Arg(c, 1), info = Arg(c, 2), len = Arg(c, 3), cls = Arg(c, 4);
  File* f = FileOf(Arg(c, 0));
  if (!f) return kStatusInvalidHandle;
  auto done = [&](uint32_t status, uint32_t information) {
    SetIoStatus(io_status, status, information);
    return status;
  };
  uint32_t need = cls == kFileDisposition ? 1 : cls == kFileRename ? 0x10 : cls == kFileBasic ? 0x28 : 8;
  if (len < need) return done(kStatusInfoLengthMismatch, 0);
  bool writes = cls != kFilePosition;
  if (writes && !f->loc.device->writable) return done(kStatusAccessDenied, 0);
  std::lock_guard<std::mutex> g(f->lock);
  switch (cls) {
    case kFilePosition: f->position = Rd64(info); break;
    case kFileEndOfFile:
    case kFileAllocation: {
      // An allocation size below the end of the file cuts it there; the
      // allocation is otherwise the file system's business.
      uint64_t v = Rd64(info);
      struct stat st;
      if (f->fd < 0 || fstat(f->fd, &st) != 0) return done(kStatusInvalidParameter, 0);
      if ((cls == kFileEndOfFile || v < uint64_t(st.st_size)) && ftruncate(f->fd, off_t(v)) != 0)
        return done(StatusFromErrno(errno), 0);
      break;
    }
    case kFileDisposition: f->delete_on_close = Rd8(info) != 0; break;
    case kFileBasic: {
      // Times only: the attributes of a host file are its own.
      uint64_t write = Rd64(info + 0x10);
      if (write && f->fd >= 0) {
        uint64_t unix100 = write - 116444736000000000ull;
        struct timespec ts[2] = {{0, UTIME_OMIT}, {time_t(unix100 / 10000000), long(unix100 % 10000000 * 100)}};
        futimens(f->fd, ts);
      }
      break;
    }
    case kFileRename: {
      // FILE_RENAME_INFORMATION: replace if it exists, a root directory, the
      // new name (ANSI_STRING).
      Location to;
      std::string name = ReadAnsiString(info + 8);
      if (uint32_t root = Rd32(info + 4)) {
        File* dir = FileOf(root);
        if (!dir) return done(kStatusInvalidHandle, 0);
        name = dir->loc.guest + "\\" + name;
      }
      uint32_t s = Resolve(name, to);
      if (s != kStatusSuccess) return done(s, 0);
      if (to.device != f->loc.device) return done(kStatusAccessDenied, 0);
      if (to.exists && !Rd32(info)) return done(kStatusObjectNameCollision, 0);
      std::error_code ec;
      std::filesystem::rename(f->loc.host, to.host, ec);
      if (ec) return done(StatusFromErrno(ec.value()), 0);
      f->loc = to;
      f->loc.exists = true;
      break;
    }
    default: ppc::Fatal(&c, "NtSetInformationFile: class %u is not supported yet", cls);
  }
  return done(kStatusSuccess, 0);
}

// NtQueryFullAttributesFile(attributes, *FILE_NETWORK_OPEN_INFORMATION).
HLE(kKrnl, 231, NtQueryFullAttributesFile) {
  Location loc;
  uint32_t status = PathOf(Arg(c, 0), loc);
  if (status != kStatusSuccess) return status;
  struct stat st;
  if (stat(loc.host.c_str(), &st) != 0) return StatusFromErrno(errno);
  PutNetworkOpen(Arg(c, 1), st, *loc.device);
  return kStatusSuccess;
}

// NtQueryVolumeInformationFile(handle, *io_status, info, length, class).
HLE(kKrnl, 239, NtQueryVolumeInformationFile) {
  uint32_t io_status = Arg(c, 1), info = Arg(c, 2), len = Arg(c, 3), cls = Arg(c, 4);
  File* f = FileOf(Arg(c, 0));
  if (!f) return kStatusInvalidHandle;
  const Device& d = *f->loc.device;
  uint32_t need = cls == kFsDevice ? 8 : cls == kFsAttribute ? 0x10 : 0x18;
  if (cls != kFsVolume && cls != kFsSize && cls != kFsDevice && cls != kFsAttribute)
    ppc::Fatal(&c, "NtQueryVolumeInformationFile: class %u is not supported yet", cls);
  if (len < need) {
    SetIoStatus(io_status, kStatusInfoLengthMismatch, 0);
    return kStatusInfoLengthMismatch;
  }
  uint32_t status = kStatusSuccess, out = need;
  switch (cls) {
    case kFsVolume:  // creation time, serial number, label length, supports objects; no label
      std::memset(Mem() + info, 0, 0x18);
      out = 0x11;
      break;
    case kFsSize: {  // total and free allocation units, sectors per unit, bytes per sector
      struct statvfs vfs;
      uint64_t total = 0, free = 0;
      if (statvfs(d.root.c_str(), &vfs) == 0) {
        total = uint64_t(vfs.f_blocks) * vfs.f_frsize / d.cluster;
        free = d.writable ? uint64_t(vfs.f_bavail) * vfs.f_frsize / d.cluster : 0;
      }
      Wr64(info, total);
      Wr64(info + 8, free);
      Wr32(info + 0x10, d.cluster / 0x200);
      Wr32(info + 0x14, 0x200);
      break;
    }
    case kFsDevice:
      Wr32(info, d.device_type);
      Wr32(info + 4, d.writable ? 0 : 0x3);  // FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE
      break;
    case kFsAttribute: {  // attributes, longest name (FATX's is 42), the file system's name
      uint32_t n = uint32_t(d.file_system.size());
      Wr32(info, 0);
      Wr32(info + 4, d.file_system == "FATX" ? 42 : 255);
      Wr32(info + 8, n);
      uint32_t room = len - 0xC;
      std::memcpy(Mem() + info + 0xC, d.file_system.data(), std::min(n, room));
      out = 0xC + std::min(n, room);
      if (room < n) status = kStatusBufferOverflow;
      break;
    }
  }
  SetIoStatus(io_status, status, out);
  return status;
}

// NtQueryDirectoryFile(handle, event, APC routine, APC context,
// *io_status, info, length, file mask (ANSI_STRING), restart): one
// FILE_DIRECTORY_INFORMATION per call, its name in ANSI at 0x40. The mask
// applies from the first query (or a restart) on.
HLE(kKrnl, 228, NtQueryDirectoryFile) {
  uint32_t handle = Arg(c, 0), event = Arg(c, 1), io_status = Arg(c, 4), info = Arg(c, 5), len = Arg(c, 6);
  uint32_t mask_ptr = Arg(c, 7);
  bool restart = Arg(c, 8) != 0;
  RefuseApc(c, Arg(c, 2), "NtQueryDirectoryFile");
  File* f = FileOf(handle);
  if (!f) return kStatusInvalidHandle;
  if (f->fd >= 0) return Complete(handle, event, io_status, kStatusNotADirectory, 0);
  if (len < 0x48) return Complete(handle, event, io_status, kStatusInfoLengthMismatch, 0);
  std::lock_guard<std::mutex> g(f->lock);
  if (!f->listed || restart) {
    std::string mask = mask_ptr ? ReadAnsiString(mask_ptr) : "";
    if (mask.empty()) mask = "*";
    f->entries.clear();
    if (DIR* dir = opendir(f->loc.host.c_str())) {
      while (dirent* e = readdir(dir)) {
        std::string n = e->d_name;
        if (n != "." && n != ".." && MatchMask(n, mask)) f->entries.push_back(n);
      }
      closedir(dir);
    }
    // In a fixed order, whatever the host's directory order.
    std::sort(f->entries.begin(), f->entries.end());
    f->next = 0;
    f->listed = true;
    if (f->entries.empty()) return Complete(handle, event, io_status, kStatusNoSuchFile, 0);
  }
  if (f->next >= f->entries.size()) return Complete(handle, event, io_status, kStatusNoMoreFiles, 0);
  const std::string& name = f->entries[f->next];
  if (0x40 + name.size() > len) return Complete(handle, event, io_status, kStatusBufferOverflow, 0);
  struct stat st{};
  stat((f->loc.host / name).c_str(), &st);
  const Device& d = *f->loc.device;
  uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
  // Next entry offset, file index, the times, size, allocation size,
  // attributes, name length, name.
  Wr32(info + 0x00, 0);
  Wr32(info + 0x04, uint32_t(f->next));
  Wr64(info + 0x08, ModifyTime(st));
  Wr64(info + 0x10, AccessTime(st));
  Wr64(info + 0x18, ModifyTime(st));
  Wr64(info + 0x20, ModifyTime(st));
  Wr64(info + 0x28, size);
  Wr64(info + 0x30, Allocation(size, d));
  Wr32(info + 0x38, FileAttributes(st, d));
  Wr32(info + 0x3C, uint32_t(name.size()));
  std::memcpy(Mem() + info + 0x40, name.data(), name.size());
  f->next++;
  return Complete(handle, event, io_status, kStatusSuccess, uint32_t(0x40 + name.size()));
}

// NtFlushBuffersFile(handle, *io_status).
HLE(kKrnl, 219, NtFlushBuffersFile) {
  File* f = FileOf(Arg(c, 0));
  if (!f) return kStatusInvalidHandle;
  if (f->fd >= 0) fsync(f->fd);
  SetIoStatus(Arg(c, 1), kStatusSuccess, 0);
  return kStatusSuccess;
}

// ObCreateSymbolicLink(link, target), ObDeleteSymbolicLink(link): ANSI_STRINGs.
HLE(kKrnl, 259, ObCreateSymbolicLink) {
  CreateSymbolicLink(ReadAnsiString(Arg(c, 0)), ReadAnsiString(Arg(c, 1)));
  return kStatusSuccess;
}
HLE(kKrnl, 260, ObDeleteSymbolicLink) {
  return DeleteSymbolicLink(ReadAnsiString(Arg(c, 0))) ? kStatusSuccess : kStatusObjectNameNotFound;
}

// FscSetCacheElementCount(count): the size of the file system cache. File
// data is cached by the host, not here, so there is nothing to size.
HLE(kKrnl, 33, FscSetCacheElementCount) { return kStatusSuccess; }

}  // namespace recomp::xbox360
