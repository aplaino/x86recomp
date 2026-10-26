#include "xbox360/fs.h"

#include <dirent.h>
#include <sys/statvfs.h>

#include <cerrno>
#include <deque>
#include <mutex>
#include <vector>

#include "xbox360/kernel.h"

namespace recomp::xbox360 {
namespace {

std::mutex g_lock;
std::deque<Device> g_devices;  // a deque: mounting keeps Location::device valid
std::vector<std::pair<std::string, std::string>> g_links;  // link (no \??\), target

char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

bool StartsWith(const std::string& s, const std::string& prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t k = 0; k < prefix.size(); k++)
    if (Lower(s[k]) != Lower(prefix[k])) return false;
  return true;
}
// prefix, followed by the end or a separator.
bool StartsWithComponent(const std::string& s, const std::string& prefix) {
  return StartsWith(s, prefix) && (s.size() == prefix.size() || s[prefix.size()] == '\\');
}

std::string StripObjectPrefix(const std::string& s) { return StartsWith(s, "\\??\\") ? s.substr(4) : s; }

// The host entry in `dir` whose name is `name` but for case; "" if none.
std::string FindEntry(const std::filesystem::path& dir, const std::string& name) {
  std::error_code ec;
  if (std::filesystem::exists(dir / name, ec)) return name;
  DIR* d = opendir(dir.c_str());
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d))
    if (SameName(e->d_name, name)) {
      found = e->d_name;
      break;
    }
  closedir(d);
  return found;
}

uint64_t ToFileTime(const struct timespec& ts) {
  return uint64_t(ts.tv_sec) * 10000000 + uint64_t(ts.tv_nsec) / 100 + 116444736000000000ull;
}

}  // namespace

void MountDevice(const Device& d) {
  std::lock_guard<std::mutex> g(g_lock);
  g_devices.push_back(d);
}

void CreateSymbolicLink(const std::string& link, const std::string& target) {
  std::lock_guard<std::mutex> g(g_lock);
  std::string name = StripObjectPrefix(link);
  for (auto& l : g_links)
    if (SameName(l.first, name)) {
      l.second = target;
      return;
    }
  g_links.emplace_back(name, target);
}

bool DeleteSymbolicLink(const std::string& link) {
  std::lock_guard<std::mutex> g(g_lock);
  std::string name = StripObjectPrefix(link);
  for (auto it = g_links.begin(); it != g_links.end(); ++it)
    if (SameName(it->first, name)) {
      g_links.erase(it);
      return true;
    }
  return false;
}

std::string SymbolicLinkTarget(const std::string& link) {
  std::lock_guard<std::mutex> g(g_lock);
  std::string name = StripObjectPrefix(link);
  for (const auto& l : g_links)
    if (SameName(l.first, name)) return l.second;
  return "";
}

uint32_t Resolve(const std::string& path, Location& out) {
  std::lock_guard<std::mutex> g(g_lock);
  std::string p = StripObjectPrefix(path);
  // Links, at most a few deep (a link to a link).
  for (int depth = 0; depth < 8; depth++) {
    const std::pair<std::string, std::string>* hit = nullptr;
    for (const auto& l : g_links)
      if (StartsWithComponent(p, l.first) && (!hit || l.first.size() > hit->first.size())) hit = &l;
    if (!hit) break;
    p = StripObjectPrefix(hit->second + p.substr(hit->first.size()));
  }
  const Device* dev = nullptr;
  for (const Device& d : g_devices)
    if (StartsWithComponent(p, d.name)) dev = &d;
  if (!dev) return kStatusObjectPathNotFound;

  // The components under the device, "." and ".." applied.
  std::vector<std::string> parts;
  std::string rest = p.substr(dev->name.size());
  for (size_t k = 0; k <= rest.size();) {
    size_t e = rest.find('\\', k);
    if (e == std::string::npos) e = rest.size();
    std::string c = rest.substr(k, e - k);
    k = e + 1;
    if (c.empty() || c == ".") continue;
    if (c == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    if (c.find_first_of("/\0", 0, 2) != std::string::npos) return kStatusObjectNameInvalid;
    parts.push_back(c);
  }
  out = {dev, dev->root, dev->name, true};
  for (size_t k = 0; k < parts.size(); k++) {
    std::string found = FindEntry(out.host, parts[k]);
    bool last = k + 1 == parts.size();
    if (found.empty()) {
      if (!last) return kStatusObjectPathNotFound;
      found = parts[k];
      out.exists = false;
    }
    out.host /= found;
    out.guest += "\\" + found;
  }
  return kStatusSuccess;
}

VolumeSize QueryVolumeSize(const Device& d) {
  struct statvfs vfs;
  if (statvfs(d.root.c_str(), &vfs) != 0) return {0, 0};
  return {uint64_t(vfs.f_blocks) * vfs.f_frsize / d.cluster,
          d.writable ? uint64_t(vfs.f_bavail) * vfs.f_frsize / d.cluster : 0};
}

uint32_t StatusFromErrno(int err) {
  switch (err) {
    case ENOENT: return kStatusObjectNameNotFound;
    case EEXIST: return kStatusObjectNameCollision;
    case EACCES: case EPERM: case EROFS: return kStatusAccessDenied;
    case ENOTDIR: return kStatusNotADirectory;
    case EISDIR: return kStatusFileIsADirectory;
    case ENOTEMPTY: return kStatusDirectoryNotEmpty;
    case ENOSPC: return kStatusDiskFull;
    case ENAMETOOLONG: return kStatusObjectNameInvalid;
    default: return kStatusUnsuccessful;
  }
}

uint64_t ModifyTime(const struct stat& st) {
#if defined(__APPLE__)
  return ToFileTime(st.st_mtimespec);
#else
  return ToFileTime(st.st_mtim);
#endif
}

uint64_t AccessTime(const struct stat& st) {
#if defined(__APPLE__)
  return ToFileTime(st.st_atimespec);
#else
  return ToFileTime(st.st_atim);
#endif
}

uint32_t FileAttributes(const struct stat& st, const Device& d) {
  constexpr uint32_t kReadOnly = 0x01, kDirectory = 0x10, kNormal = 0x80;
  uint32_t a = S_ISDIR(st.st_mode) ? kDirectory : 0;
  if (!d.writable) a |= kReadOnly;
  return a ? a : kNormal;
}

bool SameName(const std::string& a, const std::string& b) {
  return a.size() == b.size() && StartsWith(a, b);
}

bool MatchMask(const std::string& name, const std::string& mask) {
  // Iterative * and ? matching, case-insensitive.
  size_t n = 0, m = 0, star = std::string::npos, mark = 0;
  while (n < name.size()) {
    if (m < mask.size() && (mask[m] == '?' || Lower(mask[m]) == Lower(name[n]))) {
      n++;
      m++;
    } else if (m < mask.size() && mask[m] == '*') {
      star = m++;
      mark = n;
    } else if (star != std::string::npos) {
      m = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (m < mask.size() && mask[m] == '*') m++;
  return m == mask.size();
}

}  // namespace recomp::xbox360
