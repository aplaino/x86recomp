#pragma once
// The guest's file namespace (ARCHITECTURE.md, "Files"): devices, each a
// host directory, and the symbolic links titles reach them through (GAME:
// and D: to the title's disc, and what ObCreateSymbolicLink adds). Guest
// paths are case-insensitive, as the console's file systems are; host
// paths keep the case their files have.

#include <sys/stat.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace recomp::xbox360 {

struct Device {
  std::string name;             // e.g. "\Device\CdRom0"
  std::filesystem::path root;   // the host directory
  bool writable;
  std::string file_system;      // what FileFsAttributeInformation names: "GDFX", "FATX"
  uint32_t device_type;         // FileFsDeviceInformation's FILE_DEVICE_*
  uint32_t cluster;             // the file system's allocation unit
};
// FILE_DEVICE_* values.
constexpr uint32_t kFileDeviceCdRom = 0x02, kFileDeviceDisk = 0x07;

// Devices stay mounted until the process ends (open files refer to them).
void MountDevice(const Device& d);
// Links name a path prefix ("GAME:", "\??\GAME:" alike) that stands for a
// target ("\Device\CdRom0"); a link may name another.
void CreateSymbolicLink(const std::string& link, const std::string& target);
bool DeleteSymbolicLink(const std::string& link);
// A link's target, "" if there is no such link.
std::string SymbolicLinkTarget(const std::string& link);

// A guest path resolved to a device: the host path (its last component as
// the guest gave it if no entry has that name yet), the guest path in its
// canonical form (links resolved, "." and ".." applied, no trailing
// separator).
struct Location {
  const Device* device = nullptr;
  std::filesystem::path host;
  std::string guest;
  bool exists = false;
};
// kStatusSuccess, kStatusObjectPathNotFound (no device, or a directory on
// the way is missing) or kStatusObjectNameInvalid.
uint32_t Resolve(const std::string& path, Location& out);

// A device's size and free space in its allocation units
// (FileFsSizeInformation); a read-only device has none free.
struct VolumeSize {
  uint64_t total, free;
};
VolumeSize QueryVolumeSize(const Device& d);

// The NTSTATUS of a host error.
uint32_t StatusFromErrno(int err);
// A host file's times as FILETIMEs (100 ns since 1601).
uint64_t ModifyTime(const struct stat& st);
uint64_t AccessTime(const struct stat& st);
// FILE_ATTRIBUTE_* of a host file on a device.
uint32_t FileAttributes(const struct stat& st, const Device& d);

// Case-insensitive comparison of guest names, and wildcard matching with
// * and ? (NtQueryDirectoryFile's file mask).
bool SameName(const std::string& a, const std::string& b);
bool MatchMask(const std::string& name, const std::string& mask);

}  // namespace recomp::xbox360
