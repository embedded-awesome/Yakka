#include "file_check.hpp"
#include "project_diff.hpp"
#include <stdexcept>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <unordered_map>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <system_error>
#include <algorithm>
#include <vector>

namespace yakka {
namespace {

struct known_hash {
  file_stat stat;
  uint64_t hash;
};

// Hashes already known for a given stat in this process, from checks, records or seeded previous records.
std::mutex known_hashes_mutex;
std::unordered_map<std::string, known_hash> known_hashes;

std::string hash_key(const std::filesystem::path &path)
{
  return path.is_absolute() ? path.lexically_normal().generic_string() : std::filesystem::absolute(path).lexically_normal().generic_string();
}

void remember_hash(const std::filesystem::path &path, const file_stat &stat, uint64_t hash)
{
  std::lock_guard lock(known_hashes_mutex);
  known_hashes.insert_or_assign(hash_key(path), known_hash{ stat, hash });
}

std::optional<uint64_t> lookup_hash(const std::filesystem::path &path, const file_stat &stat)
{
  std::lock_guard lock(known_hashes_mutex);
  const auto found = known_hashes.find(hash_key(path));
  if (found == known_hashes.end() || !(found->second.stat == stat))
    return std::nullopt;
  return found->second.hash;
}

[[maybe_unused]] uint64_t file_hash(const std::filesystem::path &path)
{
  if (std::filesystem::is_directory(path)) {
    std::vector<std::string> entries;
    for (const auto &entry: std::filesystem::directory_iterator(path))
      entries.push_back(entry.path().filename().generic_string());
    std::sort(entries.begin(), entries.end());
    std::string names;
    for (const auto &entry: entries) {
      names += entry;
      names += '\0';
    }
    return project_diff::hash_bytes(names.data(), names.size());
  }
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("Cached input is not a regular file or directory: " + path.generic_string());
  const auto hash = project_diff::hash_file(path);
  if (!hash)
    throw std::runtime_error("Cannot hash cached input: " + path.generic_string());
  return *hash;
}

/// Hash a file whose stat was taken first, so a write during hashing leaves a stale stat and is caught next time.
uint64_t hash_with_stat(const std::filesystem::path &path, const file_stat &stat)
{
  if (const auto known = lookup_hash(path, stat))
    return *known;
  const auto hash = file_hash(path);
  remember_hash(path, stat, hash);
  return hash;
}

int64_t now_ns()
{
#ifdef _WIN32
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::file_time_type::clock::now().time_since_epoch()).count();
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#endif
}

} // namespace

std::optional<file_stat> stat_file(const std::filesystem::path &path)
{
#ifdef _WIN32
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error && error != std::errc::no_such_file_or_directory)
    throw std::filesystem::filesystem_error("Cannot check cached input", path, error);
  if (!std::filesystem::exists(status))
    return std::nullopt;
  file_stat result;
  result.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::filesystem::last_write_time(path).time_since_epoch()).count();
  result.ctime = result.mtime;
  result.size  = std::filesystem::is_regular_file(status) ? std::filesystem::file_size(path) : 0;
  return result;
#else
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return std::nullopt;
    throw std::filesystem::filesystem_error("Cannot check cached input", path, std::error_code(errno, std::generic_category()));
  }
#  ifdef __APPLE__
  const auto &modified = info.st_mtimespec;
  const auto &changed  = info.st_ctimespec;
#  else
  const auto &modified = info.st_mtim;
  const auto &changed  = info.st_ctim;
#  endif
  file_stat result;
  result.mtime = int64_t(modified.tv_sec) * 1'000'000'000 + modified.tv_nsec;
  result.ctime = int64_t(changed.tv_sec) * 1'000'000'000 + changed.tv_nsec;
  result.size  = uint64_t(info.st_size);
  result.inode = uint64_t(info.st_ino);
  return result;
#endif
}

bool file_stat_settled(const file_stat &stat)
{
  const auto limit = now_ns() - file_stat_settle_time_ns;
  return stat.mtime < limit && stat.ctime < limit;
}

std::optional<file_stat> recorded_file_stat(ryml::ConstNodeRef node)
{
  if (!node.valid() || !node.is_map() || !node.has_child("mtime") || !node.has_child("ctime") || !node.has_child("size") || !node.has_child("inode"))
    return std::nullopt;
  const auto mtime = node["mtime"].val<int64_t>();
  const auto ctime = node["ctime"].val<int64_t>();
  const auto size  = node["size"].val<uint64_t>();
  const auto inode = node["inode"].val<uint64_t>();
  if (!mtime || !ctime || !size || !inode)
    return std::nullopt;
  return file_stat{ *mtime, *ctime, *size, *inode };
}

void record_file_check(ryml::NodeRef node, const std::filesystem::path &path)
{
  node.clear_children();
  node |= ryml::MAP;
  node["policy"] << file_check_policy;
  const auto stat = stat_file(path);
  node["exists"] << stat.has_value();
  if (!stat)
    return;
  if constexpr (use_timestamp_file_checks) {
    node["timestamp"] << static_cast<int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
  } else {
    auto hash = node["hash"];
    hash << project_diff::to_hex(hash_with_stat(path, *stat));
    // Hex digests such as 791860556ee10816 would otherwise be emitted unquoted, which strict JSON parsers reject
    hash.tree()->_add_flags(hash.id(), ryml::VALQUO);
    if (file_stat_settled(*stat)) {
      node["mtime"] << stat->mtime;
      node["ctime"] << stat->ctime;
      node["size"] << stat->size;
      node["inode"] << stat->inode;
    }
  }
}

file_check_state check_file(ryml::ConstNodeRef node, const std::filesystem::path &path)
{
  if (!node.valid() || !node.is_map() || !node.has_child("policy") || !node["policy"].has_val() || node["policy"].val() != ryml::to_csubstr(file_check_policy) || !node.has_child("exists"))
    return file_check_state::changed;
  const auto existed = node["exists"].val<bool>();
  if (!existed)
    return file_check_state::changed;
  const auto stat = stat_file(path);
  if (stat.has_value() != *existed)
    return file_check_state::changed;
  if (!stat)
    return file_check_state::unchanged;
  if constexpr (use_timestamp_file_checks) {
    if (!node.has_child("timestamp"))
      return file_check_state::changed;
    const auto previous = node["timestamp"].val<int64_t>();
    return previous && *previous == static_cast<int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count()) ? file_check_state::unchanged : file_check_state::changed;
  } else {
    if (!node.has_child("hash") || !node["hash"].has_val())
      return file_check_state::changed;
    const auto previous = project_diff::from_hex(node["hash"].val());
    if (!previous)
      return file_check_state::changed;
    if (const auto recorded = recorded_file_stat(node); recorded && *recorded == *stat) {
      remember_hash(path, *stat, *previous);
      return file_check_state::unchanged;
    }
    return *previous == hash_with_stat(path, *stat) ? file_check_state::refresh : file_check_state::changed;
  }
}

bool file_check_matches(ryml::ConstNodeRef node, const std::filesystem::path &path)
{
  return check_file(node, path) != file_check_state::changed;
}

bool file_check_settled(ryml::ConstNodeRef node)
{
  if (!node.valid() || !node.is_map() || !node.has_child("exists"))
    return false;
  if (!node["exists"].val<bool>().value_or(false) || use_timestamp_file_checks)
    return true;
  return node.has_child("mtime");
}

void seed_file_check(ryml::ConstNodeRef node, const std::filesystem::path &path)
{
  if constexpr (!use_timestamp_file_checks) {
    if (!node.valid() || !node.is_map() || !node.has_child("policy") || node["policy"].val() != ryml::to_csubstr(file_check_policy) || !node.has_child("hash"))
      return;
    const auto stat = recorded_file_stat(node);
    const auto hash = project_diff::from_hex(node["hash"].val());
    if (stat && hash)
      remember_hash(path, *stat, *hash);
  }
}

} // namespace yakka
