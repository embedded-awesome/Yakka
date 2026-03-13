#pragma once

#include "yakka_blueprint.hpp"
#include "yakka.hpp"
#include "semver/semver.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>

namespace yakka {

struct component {
  yakka_status parse_file(std::filesystem::path file_path, std::filesystem::path package_path = {}, ryml::NodeRef parent_node = {});
  //std::tuple<component_list_t &, feature_list_t &> apply_feature(std::string feature_name);
  //std::tuple<component_list_t &, feature_list_t &> process_requirements(const ryml::Tree &node);
  component_list_t get_required_components();
  feature_list_t get_required_features();
  void convert_to_yakka();
  // std::vector< blueprint_node > get_blueprints();

  // Variables
  ryml::csubstr id;
  std::filesystem::path file_path;
  std::filesystem::path component_path;
  
  // - tree: May be unused if parent_node provided during parsing
  // - yaml_buffer: Buffer to hold YAML file contents (ryml uses views into this)
  ryml::Tree tree;
  std::string yaml_buffer;
  ryml::NodeRef root; // Root node reference for easy access
  ryml::NodeRef blueprints; // Helper reference to blueprints node for easy access
  
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
  // ryml::ConstNodeRef root() const { return tree.crootref(); }
  // ryml::NodeRef root() { return tree.rootref(); }
};

} /* namespace yakka */
