/**
 * @file project_diff.cpp
 * @brief Implements change detection between the previous and current project summary.
 */

#include "project_diff.hpp"
#include "yakka.hpp"
#include "utilities.hpp"

#include "../components/rapidhash/v3/rapidhash.h"

#include "spdlog/spdlog.h"

#include <array>
#include <charconv>
#include <format>

namespace yakka {

namespace {
constexpr std::array<const char *, 5> hashed_sections = { "components", "data", "tools", "features", "choices" };

uint64_t mix_hashes(uint64_t a, uint64_t b)
{
  const std::array<uint64_t, 2> values = { a, b };
  return rapidhash(values.data(), sizeof(values));
}
} // namespace

project_diff::project_diff()
{
  storage.reserve_arena(4 * 1024 * 1024);
  storage.add_flags(ryml::Tree::TREEF_NO_ARENA_REALLOC);
}

uint64_t project_diff::hash_bytes(const char *data, size_t length)
{
  return rapidhash(data, length);
}

std::optional<uint64_t> project_diff::hash_file(const std::filesystem::path &path)
{
  auto contents = yakka::get_file_contents<std::string>(path);
  if (!contents)
    return std::nullopt;
  return hash_bytes(contents->data(), contents->size());
}

std::string project_diff::to_hex(uint64_t value)
{
  return std::format("{:016x}", value);
}

std::optional<uint64_t> project_diff::from_hex(ryml::csubstr text)
{
  if (text.empty())
    return std::nullopt;
  uint64_t value = 0;
  const auto result = std::from_chars(text.str, text.str + text.len, value, 16);
  if (result.ec != std::errc{} || result.ptr != text.str + text.len)
    return std::nullopt;
  return value;
}

ryml::csubstr project_diff::intern(ryml::csubstr text)
{
  if (text.empty())
    return text;
  return storage.copy_to_arena(text);
}

void project_diff::append_pointer_token(std::string &path, ryml::csubstr token)
{
  path += '/';
  for (const char c: token) {
    if (c == '~')
      path += "~0";
    else if (c == '/')
      path += "~1";
    else
      path += c;
  }
}

uint64_t project_diff::hash_subtree(ryml::ConstNodeRef node, std::string &path, hash_map &out)
{
  uint64_t hash = 0;

  if (!node.valid())
    return hash;

  if (node.is_map()) {
    hash = 0x9e3779b97f4a7c15ull;
    for (const auto child: node.children()) {
      const auto parent_length = path.size();
      if (child.has_key())
        append_pointer_token(path, child.key());
      const uint64_t child_hash = hash_subtree(child, path, out);
      path.resize(parent_length);
      // Accumulate commutatively so map ordering does not affect the result
      hash += mix_hashes(child.has_key() ? hash_bytes(child.key().str, child.key().len) : 0, child_hash);
    }
  } else if (node.is_seq()) {
    hash = 0xc2b2ae3d27d4eb4full;
    size_t index = 0;
    for (const auto child: node.children()) {
      const auto parent_length = path.size();
      append_pointer_token(path, ryml::to_csubstr(std::to_string(index)));
      const uint64_t child_hash = hash_subtree(child, path, out);
      path.resize(parent_length);
      hash = mix_hashes(hash, child_hash);
      ++index;
    }
  } else if (node.has_val()) {
    hash = hash_bytes(node.val().str, node.val().len);
  }

  out[path] = hash;
  return hash;
}

void project_diff::take_snapshot(ryml::ConstNodeRef summary, snapshot &target)
{
  target = snapshot{};

  if (!summary.valid() || !summary.is_map())
    return;

  const auto collect_keys = [&](ryml::csubstr section, std::unordered_set<ryml::csubstr> &out) {
    if (!summary.has_child(section))
      return;
    for (const auto child: summary[section].children())
      if (child.has_key())
        out.insert(intern(child.key()));
  };

  collect_keys("components", target.components);
  collect_keys("tools", target.tools);
  collect_keys("choices", target.choices);

  if (summary.has_child("components")) {
    for (const auto child: summary["components"].children()) {
      if (!child.has_key() || !child.has_child("yakka_file"))
        continue;

      component_file_state file_state;
      file_state.yakka_file = intern(child["yakka_file"].val());
      if (child.has_child("yakka_file_timestamp")) {
        const auto timestamp = child["yakka_file_timestamp"].val<int64_t>();
        if (timestamp) {
          file_state.timestamp     = *timestamp;
          file_state.has_timestamp = true;
        }
      }
      if (child.has_child("yakka_file_hash")) {
        const auto hash = from_hex(child["yakka_file_hash"].val());
        if (hash) {
          file_state.hash     = *hash;
          file_state.has_hash = true;
        }
      }
      target.component_files.emplace(intern(child.key()), file_state);
    }
  }

  if (summary.has_child("features"))
    for (const auto child: summary["features"].children())
      if (child.has_val())
        target.features.insert(intern(child.val()));

  if (summary.has_child("initial")) {
    const auto initial = summary["initial"];
    if (initial.has_child("components"))
      for (const auto child: initial["components"].children())
        if (child.has_val())
          target.initial_components.push_back(intern(child.val()));
    if (initial.has_child("features"))
      for (const auto child: initial["features"].children())
        if (child.has_val())
          target.initial_features.push_back(intern(child.val()));
  }

  std::string path;
  for (const auto *section: hashed_sections) {
    const auto key = ryml::to_csubstr(section);
    if (!summary.has_child(key))
      continue;
    path.clear();
    append_pointer_token(path, key);
    hash_subtree(summary[key], path, target.hashes);
  }
}

void project_diff::capture_baseline(ryml::ConstNodeRef summary)
{
  take_snapshot(summary, baseline);
  had_previous_state = true;
}

void project_diff::detect_component_changes(std::filesystem::file_time_type summary_last_modified)
{
  if (!had_previous_state)
    return;

  for (auto &[name, file_state]: baseline.component_files) {
    if (file_state.yakka_file.empty()) {
      components_modified.insert(name);
      continue;
    }

    const auto yakka_file = ryml_path(file_state.yakka_file);
    std::error_code error;
    const auto last_modified = std::filesystem::last_write_time(yakka_file, error);
    if (error) {
      components_modified.insert(name);
      continue;
    }

    // Fast path: the recorded timestamp still matches, so the contents cannot have changed
    if (file_state.has_timestamp) {
      if (file_state.timestamp == static_cast<int64_t>(last_modified.time_since_epoch().count()))
        continue;
    } else if (last_modified <= summary_last_modified) {
      continue;
    }

    // Timestamp moved; fall back to content hashing so a touched file does not force a rebuild
    if (!file_state.has_hash) {
      components_modified.insert(name);
      continue;
    }

    const auto hash = hash_file(yakka_file);
    if (!hash || *hash != file_state.hash)
      components_modified.insert(name);
  }
}

void project_diff::compare_key_sets(const std::unordered_set<ryml::csubstr> &before,
                                    const std::unordered_set<ryml::csubstr> &after,
                                    const char *section,
                                    std::unordered_set<ryml::csubstr> &added,
                                    std::unordered_set<ryml::csubstr> &removed,
                                    std::unordered_set<ryml::csubstr> &modified)
{
  for (const auto name: before)
    if (!after.contains(name))
      removed.insert(name);

  std::string path;
  for (const auto name: after) {
    if (!before.contains(name)) {
      added.insert(name);
      continue;
    }
    path.clear();
    append_pointer_token(path, ryml::to_csubstr(section));
    append_pointer_token(path, name);
    if (hash_differs(path))
      modified.insert(name);
  }
}

void project_diff::finalize(ryml::ConstNodeRef summary, const std::unordered_set<ryml::csubstr> &required_components, const std::unordered_set<ryml::csubstr> &required_features)
{
  take_snapshot(summary, current);
  has_current = true;

  if (!had_previous_state) {
    components_added = current.components;
    features_added   = current.features;
    tools_added      = current.tools;
    choices_added    = current.choices;
    initial_changed  = true;
    return;
  }

  for (const auto name: baseline.components)
    if (!required_components.contains(name))
      components_removed.insert(name);

  for (const auto name: required_components) {
    const auto interned = intern(name);
    if (!baseline.components.contains(name))
      components_added.insert(interned);
    else if (!components_modified.contains(name))
      components_unchanged.insert(interned);
  }

  for (const auto name: baseline.features)
    if (!required_features.contains(name))
      features_removed.insert(name);

  for (const auto name: required_features)
    if (!baseline.features.contains(name))
      features_added.insert(intern(name));

  compare_key_sets(baseline.tools, current.tools, "tools", tools_added, tools_removed, tools_modified);
  compare_key_sets(baseline.choices, current.choices, "choices", choices_added, choices_removed, choices_modified);

  initial_changed = baseline.initial_components != current.initial_components || baseline.initial_features != current.initial_features;
}

bool project_diff::hash_differs(const std::string &path) const
{
  if (!had_previous_state || !has_current)
    return true;

  const auto before = baseline.hashes.find(path);
  const auto after  = current.hashes.find(path);

  // A path that resolves in neither summary contributes nothing to either build
  if (before == baseline.hashes.end() && after == current.hashes.end())
    return false;

  if (before == baseline.hashes.end() || after == current.hashes.end())
    return true;

  return before->second != after->second;
}

bool project_diff::has_data_changed(ryml::csubstr data_path) const
{
  if (!had_previous_state || !has_current)
    return true;

  if (data_path.empty() || data_path[0] != data_dependency_identifier)
    return false;

  const auto path = data_path.sub(1);
  if (path.empty() || path[0] != '/')
    return true;

  // Splits "<token>/<rest>" leaving the leading '/' on the remainder
  const auto split = [](ryml::csubstr text) {
    const auto slash = text.find('/');
    if (slash == ryml::csubstr::npos)
      return std::make_pair(text, ryml::csubstr{});
    return std::make_pair(text.first(slash), text.sub(slash));
  };

  auto [section, remaining] = split(path.sub(1));

  // Everything other than the project data section names a component, or all of them via the wildcard
  if (section == "data")
    return hash_differs(std::string(path.str, path.len));

  // Blueprints address component data as ':/components/<name>/...'. The section prefix is optional
  // so ':/<name>/...' remains valid.
  if (section == "components") {
    if (remaining.empty())
      return true;
    const auto split_remaining = split(remaining.sub(1));
    section                    = split_remaining.first;
    remaining                  = split_remaining.second;
  }

  const auto component_path = [&](ryml::csubstr name) {
    std::string result = "/components";
    append_pointer_token(result, name);
    result.append(remaining.str, remaining.len);
    return result;
  };

  if (section.len == 1 && section[0] == data_wildcard_identifier) {
    if (!components_added.empty() || !components_removed.empty())
      return true;
    for (const auto name: current.components)
      if (hash_differs(component_path(name)))
        return true;
    return false;
  }

  return hash_differs(component_path(section));
}

bool project_diff::empty() const
{
  if (!had_previous_state)
    return false;

  return components_added.empty() && components_removed.empty() && components_modified.empty() && features_added.empty() && features_removed.empty() && tools_added.empty() && tools_removed.empty() &&
         tools_modified.empty() && choices_added.empty() && choices_removed.empty() && choices_modified.empty() && !initial_changed;
}

std::string project_diff::describe() const
{
  if (!had_previous_state)
    return "first run";
  if (empty())
    return "no changes";

  return std::format("components +{}/-{}/~{}, features +{}/-{}, tools +{}/-{}/~{}, choices +{}/-{}/~{}, initial {}",
                     components_added.size(),
                     components_removed.size(),
                     components_modified.size(),
                     features_added.size(),
                     features_removed.size(),
                     tools_added.size(),
                     tools_removed.size(),
                     tools_modified.size(),
                     choices_added.size(),
                     choices_removed.size(),
                     choices_modified.size(),
                     initial_changed ? "changed" : "unchanged");
}

} // namespace yakka
