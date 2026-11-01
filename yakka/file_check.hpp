#pragma once

#include <ryml.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef YAKKA_FILE_CHECK_TIMESTAMP
#define YAKKA_FILE_CHECK_TIMESTAMP 0
#endif

namespace yakka {

inline constexpr bool use_timestamp_file_checks = YAKKA_FILE_CHECK_TIMESTAMP != 0;
inline constexpr const char *file_check_policy  = use_timestamp_file_checks ? "timestamp" : "hash";

struct template_file_inputs {
  std::set<std::string> files;
  std::set<std::vector<std::string>> globs;
};

/// Filesystem metadata used to skip hashing files that have not been touched, as git's index does.
struct file_stat {
  int64_t mtime  = 0; // nanoseconds
  int64_t ctime  = 0; // nanoseconds; changes on any write or timestamp update, so resetting mtime is still detected
  uint64_t size  = 0;
  uint64_t inode = 0;
  bool operator==(const file_stat &) const = default;
};

/// Stat a path without following errors; nullopt when it does not exist.
std::optional<file_stat> stat_file(const std::filesystem::path &path);

/// A stat is only trusted once both timestamps are older than this, so a later write in the same timestamp tick cannot go unnoticed.
inline constexpr int64_t file_stat_settle_time_ns = 2'000'000'000;

/// True when the stat is old enough to be trusted as a fingerprint.
bool file_stat_settled(const file_stat &stat);

/// Record existence and the compiled-in fingerprint, including expected missing files.
/// Hash records also store the file's stat once it has settled, so later checks can skip hashing.
void record_file_check(ryml::NodeRef node, const std::filesystem::path &path);

enum class file_check_state {
  changed,
  unchanged,
  refresh, // Content unchanged, but the record has no matching settled stat, so recording it again avoids rehashing next time
};

/// Missing or incompatible metadata is stale; filesystem/read errors are reported by exception.
/// Hash records with a matching settled stat are accepted without reading the file.
file_check_state check_file(ryml::ConstNodeRef node, const std::filesystem::path &path);
bool file_check_matches(ryml::ConstNodeRef node, const std::filesystem::path &path);

/// True when the record is final: the file is missing, the policy is timestamp, or the stat has settled.
/// Unsettled records are rehashed on every check until they are recorded again.
bool file_check_settled(ryml::ConstNodeRef node);

/// Remember a previous record's stat and hash so recording an unchanged file does not hash it again.
void seed_file_check(ryml::ConstNodeRef node, const std::filesystem::path &path);

/// Read the stat stored in a hash record, if it has one.
std::optional<file_stat> recorded_file_stat(ryml::ConstNodeRef node);

} // namespace yakka
