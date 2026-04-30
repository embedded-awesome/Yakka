#include "yakka_component.hpp"
// #include "yakka_schema.hpp"
#include "utilities.hpp"
#include "toml_parser_ryml.hpp"
#include "spdlog/spdlog.h"
#include "semver/semver.hpp"
#include <fstream>

using namespace semver::literals;

namespace yakka {
namespace {

bool has_component_toml_extension(const std::filesystem::path &file_path)
{
  const auto filename = file_path.filename().string();
  return filename.ends_with(yakka_component_toml_extension);
}

std::string component_id_from_path(const std::filesystem::path &file_path)
{
  const auto filename = file_path.filename().string();
  if (filename.ends_with(yakka_component_toml_extension)) {
    return filename.substr(0, filename.size() - yakka_component_toml_extension.size());
  }
  return file_path.stem().string();
}

} // namespace

yakka_status component::parse_file(std::filesystem::path file_path, std::filesystem::path package_path, ryml::NodeRef parent_node)
{
  this->file_path         = file_path;
  this->package_path      = package_path;
  std::string path_string = file_path.generic_string();
  spdlog::info("Parsing '{}'", path_string);

  try {
    // Read file contents into buffer (ryml uses views, so we need to keep the buffer)
    auto result = get_file_contents<std::string>(file_path);
    if (!result) {
      spdlog::error("Failed to read file: '{}'", path_string);
      return yakka_status::FAIL;
    }
    yaml_buffer = std::move(result.value());

    const bool is_toml_component = has_component_toml_extension(file_path);

    if (is_toml_component) {
      tree = toml_ryml::parse_toml(yaml_buffer, path_string);
      if (parent_node.valid()) {
        merge_nodes(parent_node, tree.crootref());
        root = parent_node;
      } else {
        root = tree.rootref();
      }
    } else {
      // Parse with ryml (zero-copy parser)
      if (parent_node.valid()) {
        ryml::parse_in_arena(c4::to_csubstr(yaml_buffer), parent_node);
        root = parent_node;
      } else {
        tree = ryml::parse_in_place(c4::to_substr(yaml_buffer));
        root = tree.rootref();
      }
    }

  } catch (std::exception &e) {
    spdlog::error("Failed to load file: '{}'\n{}\n", path_string, e.what());
    std::cerr << "Failed to parse: " << path_string << "\n" << e.what() << "\n";
    return yakka_status::FAIL;
  }

  // Check if file is an omap (sequence of single-key maps). Convert it to a map
  if (root.is_seq()) {
    auto new_root = root.append_sibling();
    // ryml::Tree new_tree;
    // auto new_root = new_tree.rootref();
    new_root |= ryml::MAP;

    for (const auto &item: root.children()) {
      if (item.is_map() && item.num_children() == 1) {
        for (const auto &kv: item.children()) {
          auto child = new_root.append_child();
          child.set_key(kv.key());
          if (kv.has_val()) {
            child.set_val(kv.val());
          } else if (kv.is_map() || kv.is_seq()) {
            // TODO: Need to properly copy the subtree
          }
        }
      }
    }
    root.clear();
    root = new_root;
  }

  if (file_path.filename().extension() == slce_component_extension) {
    this->type = SLCE_FILE;
    convert_to_yakka();
  } else if (file_path.filename().extension() == slcc_component_extension) {
    this->type = SLCC_FILE;

    bool result = yakka_schema_validator::get().validate(this);
    if (!result) {
      return yakka_status::FAIL;
    }

    convert_to_yakka();
  } else if (file_path.filename().extension() == slcp_component_extension) {
    this->type = SLCP_FILE;
    convert_to_yakka();
  } else {
    this->type = YAKKA_FILE;
    // Validate basic Yakka data
    bool result = yakka_schema_validator::get().validate(this);
    if (!result) {
      return yakka_status::FAIL;
    }

    if (file_path.has_parent_path())
      component_path = file_path.parent_path();
    else
      component_path = ".";
    path_string = component_path.generic_string();

    // Add directory to tree
    auto dir_node = root.append_child();
    dir_node << ryml::key("directory");
    dir_node << path_string;

    // Set version
    if (root.has_child("version")) {
      auto version_node = root.find_child("version");
      try {
        this->version = semver::version::parse(version_node.val<std::string>().value());
      } catch (std::exception &e) {
        spdlog::error("Failed to parse version: '{}'\n{}\n", version_node.val<std::string>().value(), e.what());
        this->version = "0.0.0"_v;
      }
    } else {
      this->version = "0.0.0"_v;
    }

    // Ensure certain nodes are sequences
    if (root.has_child("requires")) {
      auto requires_node = root.find_child("requires");

      if (requires_node.has_child("components")) {
        auto components_node = requires_node.find_child("components");
        // If it's a scalar, convert to sequence
        if (components_node.is_val()) {
          std::string value = components_node.val<std::string>().value();
          components_node |= ryml::SEQ;
          auto child = components_node.append_child();
          child << value;
        }
      }

      if (requires_node.has_child("features")) {
        auto features_node = requires_node.find_child("features");
        // If it's a scalar, convert to sequence
        if (features_node.is_val()) {
          std::string value = features_node.val<std::string>().value();
          features_node |= ryml::SEQ;
          auto child = features_node.append_child();
          child << value;
        }
      }

      // Fix relative component addressing
      if (requires_node.has_child("components")) {
        auto components_node = requires_node.find_child("components");
        if (components_node.is_seq()) {
          for (auto comp: components_node.children()) {
            if (comp.has_val()) {
              std::string comp_str = comp.val<std::string>().value();
              if (!comp_str.empty() && comp_str.front() == '.') {
                comp << (path_string + comp_str);
              }
            }
          }
        }
      }
    }

    // Handle supports section similarly
    if (root.has_child("supports")) {
      auto supports_node = root.find_child("supports");

      if (supports_node.has_child("features")) {
        auto features_node = supports_node.find_child("features");
        if (features_node.is_map()) {
          for (auto feature: features_node.children()) {
            if (feature.has_child("requires") && feature["requires"].has_child("components")) {
              auto components_node = feature["requires"].find_child("components");
              if (components_node.is_seq()) {
                for (auto comp: components_node.children()) {
                  if (comp.has_val()) {
                    std::string comp_str = comp.val<std::string>().value();
                    if (!comp_str.empty() && comp_str.front() == '.') {
                      comp << (path_string + comp_str);
                    }
                  }
                }
              }
            }
          }
        }
      }

      if (supports_node.has_child("components")) {
        auto components_node = supports_node.find_child("components");
        if (components_node.is_map()) {
          for (auto component: components_node.children()) {
            if (component.has_child("requires") && component["requires"].has_child("components")) {
              auto req_components_node = component["requires"].find_child("components");
              if (req_components_node.is_seq()) {
                for (auto comp: req_components_node.children()) {
                  if (comp.has_val()) {
                    std::string comp_str = comp.val<std::string>().value();
                    if (!comp_str.empty() && comp_str.front() == '.') {
                      comp << (path_string + comp_str);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    this->root["id"] << component_id_from_path(file_path);
    this->id = this->root["id"].val();
  }

  // Add known information
  auto yakka_file_node = root.append_child();
  yakka_file_node << ryml::key("yakka_file");
  yakka_file_node << file_path.string();

  // Populate json cache for backward compatibility
  // json = ryml_to_json(tree.rootref());
  // json_cache_valid = true;

  return yakka_status::SUCCESS;
}

void component::convert_to_yakka()
{
  auto root = tree.rootref();

  // Set basic data such as directory and name
  if (root.has_child("id")) {
    auto id_node       = root.find_child("id");
    std::string id_val = id_node.val<std::string>().value();

    // Set name
    if (!root.has_child("name")) {
      auto name_node = root.append_child();
      name_node << ryml::key("name");
      name_node << id_val;
    }
    this->id = id_node.val();
  } else {
    root["id"] << file_path.stem().string();
    this->id = root["id"].val();

    auto name_node = root.append_child();
    name_node << ryml::key("name");
    root["name"] << this->id;
  }

  if (root.has_child("component_root_path")) {
    std::string temp_path;
    if (!package_path.empty())
      temp_path = package_path.string() + "/";
    auto root_path_node = root.find_child("component_root_path");
    component_path      = temp_path + root_path_node.val<std::string>().value();
  } else if (root.has_child("root_path")) {
    std::string temp_path;
    if (!package_path.empty())
      temp_path = package_path.string() + "/";
    auto root_path_node = root.find_child("root_path");
    component_path      = temp_path + root_path_node.val<std::string>().value();
  } else {
    if (package_path.empty())
      if (this->type == SLCP_FILE)
        component_path = file_path.parent_path();
      else
        component_path = "./";
    else {
      if (this->type == SLCP_FILE)
        component_path = file_path.parent_path();
      else
        component_path = package_path;
    }
  }

  // Set or update directory
  if (root.has_child("directory")) {
    auto dir_node = root.find_child("directory");
    dir_node << component_path.string();
  } else {
    auto dir_node = root.append_child();
    dir_node << ryml::key("directory");
    dir_node << component_path.string();
  }

  // Process 'provides' - convert from array format to features format
  if (root.has_child("provides")) {
    auto provides_node = root.find_child("provides");
    if (provides_node.is_seq()) {
      // Create new provides structure
      ryml::Tree temp_tree;
      auto temp_root = temp_tree.rootref();
      temp_root |= ryml::MAP;
      auto features_node = temp_root.append_child();
      features_node << ryml::key("features");
      features_node |= ryml::SEQ;

      for (const auto &p: provides_node.children()) {
        if (p.is_map() && p.has_child("name")) {
          auto name_val = p.find_child("name");

          if (p.has_child("condition")) {
            // TODO: Handle conditional provides - needs ryml::Pointer equivalent
            // For now, just add to features
            auto feat_child = features_node.append_child();
            feat_child << name_val.val();
          } else {
            auto feat_child = features_node.append_child();
            feat_child << name_val.val();
          }
        }
      }

      // Replace old provides with new structure
      provides_node = temp_root;
    }
  }

  // Process 'requires' - convert from array format to features format
  if (root.has_child("requires")) {
    auto requires_node = root.find_child("requires");
    if (requires_node.is_seq()) {
      // Create new requires structure
      ryml::Tree temp_tree;
      auto temp_root = temp_tree.rootref();
      temp_root |= ryml::MAP;
      auto features_node = temp_root.append_child();
      features_node << ryml::key("features");
      features_node |= ryml::SEQ;

      for (const auto &p: requires_node.children()) {
        if (p.is_map() && p.has_child("name")) {
          auto name_val = p.find_child("name");

          if (p.has_child("condition")) {
            // TODO: Handle conditional requires
            auto feat_child = features_node.append_child();
            feat_child << name_val.val();
          } else {
            auto feat_child = features_node.append_child();
            feat_child << name_val.val();
          }
        }
      }

      // Replace old requires with new structure
      requires_node = temp_root;
    }
  }

  // Process 'component' (only available for slcp)
  if (root.has_child("component")) {
    auto component_node = root.find_child("component");
    if (component_node.is_seq()) {
      // Ensure requires/components exists
      if (!root.has_child("requires")) {
        auto req_node = root.append_child();
        req_node << ryml::key("requires");
        req_node |= ryml::MAP;
      }
      auto requires_node = root.find_child("requires");

      if (!requires_node.has_child("components")) {
        auto comp_node = requires_node.append_child();
        comp_node << ryml::key("components");
        comp_node |= ryml::SEQ;
      }
      auto req_components_node = requires_node.find_child("components");

      for (const auto &p: component_node.children()) {
        if (p.is_map() && p.has_child("id")) {
          auto id_val = p.find_child("id");

          // Add to requires/components
          auto comp_child = req_components_node.append_child();
          comp_child << id_val.val();

          // Handle instances
          if (p.has_child("instance")) {
            //auto instance_node = p.find_child("instance");
            // TODO: Handle instances - need to create instances map structure
          }
        }
      }
    }
  }

  // Update json cache after modifications
  // json = ryml_to_json(tree.rootref());
  // json_cache_valid = true;
}

} /* namespace yakka */
