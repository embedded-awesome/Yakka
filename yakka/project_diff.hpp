#pragma once

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yakka {

/**
 * @brief Describes what changed between the project summary left behind by a previous run and the
 * summary produced by the current run.
 *
 * A baseline snapshot (name sets plus content hashes) is taken from the loaded summary before it is
 * regenerated, so only one summary document ever exists.
 */
class project_diff {
public:
  struct component_file_state {
    ryml::csubstr yakka_file;
    int64_t timestamp = 0;
    uint64_t hash     = 0;
    bool has_timestamp = false;
    bool has_hash      = false;
  };

  project_diff();

  /// Snapshot a summary loaded from a previous run. Must be called before the summary is modified.
  void capture_baseline(ryml::ConstNodeRef summary);

  /// Compare each baseline component's source file against the recorded timestamp and hash.
  void detect_component_changes(std::filesystem::file_time_type summary_last_modified);

  /// Compute the final diff once the current summary has been fully regenerated.
  void finalize(ryml::ConstNodeRef summary, const std::unordered_set<ryml::csubstr> &required_components, const std::unordered_set<ryml::csubstr> &required_features);

  [[nodiscard]] bool has_data_changed(ryml::csubstr data_path) const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::string describe() const;

  static uint64_t hash_bytes(const char *data, size_t length);
  static std::optional<uint64_t> hash_file(const std::filesystem::path &path);
  static std::string to_hex(uint64_t value);
  static std::optional<uint64_t> from_hex(ryml::csubstr text);

  bool had_previous_state = false;
  bool initial_changed    = false;

  std::unordered_set<ryml::csubstr> components_added;
  std::unordered_set<ryml::csubstr> components_removed;
  std::unordered_set<ryml::csubstr> components_modified;
  std::unordered_set<ryml::csubstr> components_unchanged;
  std::unordered_set<ryml::csubstr> features_added;
  std::unordered_set<ryml::csubstr> features_removed;
  std::unordered_set<ryml::csubstr> tools_added;
  std::unordered_set<ryml::csubstr> tools_removed;
  std::unordered_set<ryml::csubstr> tools_modified;
  std::unordered_set<ryml::csubstr> choices_added;
  std::unordered_set<ryml::csubstr> choices_removed;
  std::unordered_set<ryml::csubstr> choices_modified;

private:
  using hash_map = std::unordered_map<std::string, uint64_t>;

  struct snapshot {
    std::unordered_set<ryml::csubstr> components;
    std::unordered_set<ryml::csubstr> features;
    std::unordered_set<ryml::csubstr> tools;
    std::unordered_set<ryml::csubstr> choices;
    std::vector<ryml::csubstr> initial_components;
    std::vector<ryml::csubstr> initial_features;
    std::unordered_map<ryml::csubstr, component_file_state> component_files;
    hash_map hashes;
  };

  ryml::csubstr intern(ryml::csubstr text);
  void take_snapshot(ryml::ConstNodeRef summary, snapshot &target);
  static uint64_t hash_subtree(ryml::ConstNodeRef node, std::string &path, hash_map &out);
  static void append_pointer_token(std::string &path, ryml::csubstr token);
  [[nodiscard]] bool hash_differs(const std::string &path) const;
  void compare_key_sets(const std::unordered_set<ryml::csubstr> &before,
                        const std::unordered_set<ryml::csubstr> &after,
                        const char *section,
                        std::unordered_set<ryml::csubstr> &added,
                        std::unordered_set<ryml::csubstr> &removed,
                        std::unordered_set<ryml::csubstr> &modified);

  ryml::Tree storage;
  snapshot baseline;
  snapshot current;
  bool has_current = false;
};

} // namespace yakka
