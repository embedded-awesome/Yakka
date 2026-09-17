/**
 * @file target_database.cpp
 * @brief Implements target lookup and blueprint match caching for project commands.
 */

#include "target_database.hpp"
#include "blueprint_database.hpp"
#include "inja.hpp"
#include "utilities.hpp"
#include "yakka.hpp"

#include <regex>
#include <fstream>
#include <algorithm>

namespace yakka {

/// @brief Executes add_target.

const std::vector<std::shared_ptr<blueprint_match>>& target_database::add_target(ryml::csubstr target, blueprint_database &blueprint_database, ryml::ConstNodeRef project_summary)
{
  // Check if target is not in the database. Note task_database is a multimap

  if (targets.find(target) == targets.end()) {
    const auto match = blueprint_database.find_match(target, project_summary);
    targets.insert({ target, match });
  }
  return targets[target];
}

yakka_status target_database::load(const std::filesystem::path file_path, const blueprint_database &blueprint_database)
{
  targets.clear();

  if (!std::filesystem::exists(file_path))
    return yakka_status::FILE_NOT_FOUND;

  auto loaded_database = ryml_load_file(file_path);
  if (!loaded_database)
    return yakka_status::LOAD_FAILED;

  database = std::move(loaded_database.value());

  const auto root = database.rootref();
  if (!root.has_child("targets") || !root["targets"].is_seq())
    return yakka_status::LOAD_FAILED;

  for (const auto &target_entry: root["targets"].children()) {
    if (!target_entry.has_child("target") || !target_entry.has_child("matches") || !target_entry["matches"].is_seq())
      continue;

    const auto key = target_entry["target"].val();
    std::vector<std::shared_ptr<blueprint_match>> matches;

    for (const auto &match_entry: target_entry["matches"].children()) {
      auto match = std::make_shared<blueprint_match>();

      if (match_entry.has_child("dependencies") && match_entry["dependencies"].is_seq()) {
        for (const auto &dependency_entry: match_entry["dependencies"].children())
          match->dependencies.push_back(dependency_entry.val());
      }

      if (match_entry.has_child("regex_matches") && match_entry["regex_matches"].is_seq()) {
        for (const auto &regex_match_entry: match_entry["regex_matches"].children())
          match->regex_matches.push_back(regex_match_entry.val());
      }

      if (match_entry.has_child("blueprint") && match_entry["blueprint"].is_map()) {
        const auto blueprint_entry = match_entry["blueprint"];
        const auto blueprint_target = blueprint_entry.has_child("target") ? blueprint_entry["target"].val() : c4::csubstr{};
        const auto parent_path      = blueprint_entry.has_child("parent_path") ? blueprint_entry["parent_path"].val() : c4::csubstr{};
        const auto blueprint_range = blueprint_database.blueprints.equal_range(blueprint_target);
        const auto blueprint_it    = std::find_if(blueprint_range.first, blueprint_range.second, [parent_path](const auto &entry) {
          return entry.second->parent_path == parent_path;
        });
        if (blueprint_it == blueprint_range.second)
          return yakka_status::LOAD_FAILED;

        match->blueprint = blueprint_it->second;
      }

      matches.push_back(match);
    }

    targets.insert({ key, std::move(matches) });
  }
  return yakka_status::SUCCESS;
}

yakka_status target_database::save(const std::filesystem::path file_path)
{
  std::filesystem::create_directories(file_path.parent_path());

  ryml::Tree output;
  output.reserve_arena(512 * 1024);
  auto root = output.rootref();
  root.set_map();

  auto targets_node = root.append_child();
  targets_node.save_key("targets");
  targets_node.set_seq();

  for (const auto &entry: targets) {
    auto target_entry = targets_node.append_child();
    target_entry.set_map();
    target_entry["target"] << entry.first;

    auto matches_node = target_entry.append_child();
    matches_node.save_key("matches");
    matches_node.set_seq();

    for (const auto &match: entry.second) {
      auto match_node = matches_node.append_child();
      match_node.set_map();

      auto dependencies_node = match_node.append_child();
      dependencies_node.save_key("dependencies");
      dependencies_node.set_seq();
      for (const auto &dependency: match->dependencies)
        dependencies_node.append_child() << dependency;

      auto regex_matches_node = match_node.append_child();
      regex_matches_node.save_key("regex_matches");
      regex_matches_node.set_seq();
      for (const auto &regex_match: match->regex_matches)
        regex_matches_node.append_child() << regex_match;

      if (match->blueprint) {
        auto blueprint_node = match_node.append_child();
        blueprint_node.save_key("blueprint");
        blueprint_node.set_map();
        blueprint_node["target"] << match->blueprint->target;
        blueprint_node["parent_path"] << match->blueprint->parent_path;
      }
    }
  }

  std::ofstream file(file_path);
  file << ryml::emitrs_json<std::string>(output);
  return yakka_status::SUCCESS;
}

} // namespace yakka
