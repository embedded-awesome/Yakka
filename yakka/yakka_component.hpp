#pragma once

#include "yakka_blueprint.hpp"
#include "yakka.hpp"
#include "semver/semver.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>

namespace yakka {

struct component {
  yakka_status parse_file(std::filesystem::path file_path, std::filesystem::path package_path = {});
  //std::tuple<component_list_t &, feature_list_t &> apply_feature(std::string feature_name);
  //std::tuple<component_list_t &, feature_list_t &> process_requirements(const nlohmann::json &node);
  component_list_t get_required_components();
  feature_list_t get_required_features();
  void convert_to_yakka();
  // std::vector< blueprint_node > get_blueprints();

  // Variables
  std::string id;
  std::filesystem::path file_path;
  std::filesystem::path component_path;
  
  // Dual storage approach for gradual migration:
  // - tree: ryml::Tree for efficient zero-copy storage (primary)
  // - json: nlohmann::json for compatibility with existing code (lazy-evaluated cache)
  // - yaml_buffer: Buffer to hold YAML file contents (ryml uses views into this)
  ryml::Tree tree;
  std::string yaml_buffer;
  mutable nlohmann::json json;  // mutable to allow lazy evaluation in const methods
  mutable bool json_cache_valid = false;  // Track if json cache is up-to-date
  
  semver::version version;

  // Optional path to package
  std::filesystem::path package_path;

  enum {
    YAKKA_FILE,
    SLCC_FILE,
    SLCP_FILE,
    SLCE_FILE,
  } type;
  
  // Helper to get root node for easy access
  ryml::ConstNodeRef root() const { return tree.rootref(); }
  ryml::NodeRef root() { json_cache_valid = false; return tree.rootref(); }
  
  // Get json representation (lazy conversion from ryml if needed)
  const nlohmann::json& get_json() const;
  nlohmann::json& get_json_mutable();
  
  // Invalidate json cache when tree is modified
  void invalidate_json_cache() { json_cache_valid = false; }
};

} /* namespace yakka */
