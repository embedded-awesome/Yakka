#include "yakka.hpp"
#include "component_database.hpp"
#include "spdlog/spdlog.h"
#include "ryml.hpp"
#include "ryml_std.hpp"
#include <c4/yml/emit.hpp>
#include "utilities.hpp"
#include "yakka.hpp"
#include <ranges>
#include <format>
#include <fstream>

namespace yakka {

static void process_blueprint(ryml::Tree &database, std::string_view id_string, const c4::yml::ConstNodeRef &blueprint_node);
namespace fs = std::filesystem;
using error  = std::error_code;

static void initialize_database(ryml::Tree &database)
{
  database.clear();
  auto root = database.rootref();
  root |= ryml::MAP;

  auto ensure_map = [&](const char *key) {
    ryml::NodeRef child = root.append_child();
    child << ryml::key(key);
    child |= ryml::MAP;
  };

  ensure_map("blueprints");
  ensure_map("components");
  ensure_map("features");
  ensure_map("types");
  ensure_map("serve");
}

// Constructor initializes empty database with default values
component_database::component_database() : workspace_path(""), database_is_dirty(false), has_scanned(false)
{
}

// Destructor saves if dirty
component_database::~component_database()
{
  if (database_is_dirty) {
    auto result = save();
    if (!result) {
      spdlog::error("Failed to save database: {}", result.error().message());
    }
  }
}

void component_database::insert(std::string_view id, const path &config_file)
{
  auto components_node = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "components", std::string{ id } }, true);
  if (!components_node.is_seq()) {
    components_node |= ryml::SEQ;
  }
  auto entry = components_node.append_child();
  entry.set_val(c4::to_csubstr(config_file.generic_string()));
  database_is_dirty = true;
}

std::expected<void, error> component_database::load(const path &workspace_path)
{
  this->workspace_path = workspace_path;
  database_filename    = this->workspace_path / yakka::database_filename;

  try {
    if (!fs::exists(database_filename)) {
      initialize_database(database);
      scan_for_components(this->workspace_path);
      return save();
    }

    auto loaded = ryml_load_file(database_filename);
    if (!loaded) {
      initialize_database(database);
      scan_for_components(this->workspace_path);
      return save();
    }
    database = std::move(*loaded);

    if (!database.crootref().has_child("components")) {
      initialize_database(database);
      scan_for_components(this->workspace_path);
      return save();
    }
    return {};
  } catch (const std::exception &e) {
    spdlog::error("Could not load component database at {}", this->workspace_path.string());
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
}

std::expected<void, error> component_database::save() const
{
  try {
    std::ofstream ofs(database_filename);
    ofs << ryml::emitrs_json<std::string>(database);
    return {};
  } catch (const std::exception &) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
}

void component_database::erase() noexcept
{
  if (!database_filename.empty()) {
    std::error_code ec;
    fs::remove(database_filename, ec);
  }
}

void component_database::clear() noexcept
{
  database.clear();
  database_is_dirty = true;
}

std::expected<bool, error> component_database::add_component(std::string_view component_id, const path &path)
{
  auto abs_path = fs::absolute(path);

  if (!fs::exists(abs_path)) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const auto path_string = abs_path.generic_string();
  const auto id_str      = std::string{ component_id };

  auto components_node = ryml_get_child(database.crootref(), "components");
  if (components_node.valid() && components_node.has_child(c4::to_csubstr(id_str))) {
    auto entries = ryml_get_child(components_node, c4::to_csubstr(id_str));
    if (entries.is_seq()) {
      for (const auto &entry : entries.children()) {
        if (ryml_val_string(entry) == path_string) {
          return false;
        }
      }
    } else if (entries.has_val() && ryml_val_string(entries) == path_string) {
      return false;
    }
  }

  auto entry_node = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "components", id_str }, true);
  if (!entry_node.is_seq()) {
    entry_node |= ryml::SEQ;
  }
  auto new_entry = entry_node.append_child();
  new_entry.set_val(c4::to_csubstr(path_string));
  database_is_dirty = true;
  return true;
}

void component_database::scan_for_components(std::optional<path> search_start_path)
{
  const auto scan_path = search_start_path.value_or(workspace_path);

  if (!fs::exists(scan_path)) {
    return;
  }

  const auto process_entry = [this](const fs::directory_entry &entry) {
    const auto &path = entry.path();
    const auto ext   = path.extension();
    const auto id    = path.stem().string();

    if (auto result = add_component(id, path); result && *result) {
      if (ext == yakka_component_extension || ext == yakka_component_old_extension) {
        parse_yakka_file(path, id);
      } else if (ext == slcc_component_extension) {
        spdlog::info("Found {}", path.string());
        parse_slcc_file(path);
      } else if (ext == slcp_component_extension) {
        spdlog::info("Found project '{}'", path.string());
        // parse_slcp_file(path);
      }
    }
  };

  auto entries = fs::recursive_directory_iterator(scan_path) | std::views::filter([](const auto &e) {
                   std::error_code ec;
                   fs::perms permissions = fs::status(e.path()).permissions();
                   if ((permissions & fs::perms::owner_read) == fs::perms::none) {
                     return false;
                   }
                   return e.exists(ec) && !e.is_directory() && e.path().filename().string().front() != '.'
                          && (e.path().extension() == yakka::yakka_component_extension || e.path().extension() == yakka::yakka_component_old_extension || e.path().extension() == yakka::slcc_component_extension
                              || e.path().extension() == yakka::slcp_component_extension);
                 });
  try {
    std::ranges::for_each(entries, process_entry);
  } catch (const std::filesystem::filesystem_error &e) {
    spdlog::error("Error scanning for components: {}. See '{}'", e.what(), e.path1().string());
  }
  has_scanned = true;
}

const path &component_database::get_path() const noexcept
{
  return workspace_path;
}

std::expected<path, error> component_database::get_component(std::string_view id, flag flags) const
{
  auto components_node = ryml_get_child(database.crootref(), "components");
  if (!components_node.valid() || !components_node.has_child(c4::to_csubstr(std::string{ id }))) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const auto node = ryml_get_child(components_node, c4::to_csubstr(std::string{ id }));
  auto iterate_node = [&](const ryml::ConstNodeRef &entry) -> std::optional<path> {
    const auto path = this->workspace_path / std::filesystem::path{ ryml_val_string(entry) };
    const auto extension = path.extension();
    if (flags == flag::IGNORE_ALL_SLC && ((extension == slcc_component_extension) || (extension == slce_component_extension) || (extension == slcp_component_extension)))
      return std::nullopt;
    if (flags == flag::IGNORE_YAKKA && extension == yakka_component_extension)
      return std::nullopt;
    if (flags == flag::ONLY_SLCC && ((extension == slce_component_extension) || (extension == slcp_component_extension)))
      return std::nullopt;
    // If there is an SLCP and there is more than one entry, ignore the SLCP
    if (extension == slcp_component_extension && node.num_children() > 1)
      return std::nullopt;
    if (fs::exists(path)) {
      return path;
    } else {
      spdlog::error("Couldn't find {}", path.string());
      return std::nullopt;
    }
  };

  if (node.is_seq()) {
    for (const auto &n : node.children()) {
      auto candidate = iterate_node(n);
      if (candidate.has_value()) {
        return candidate.value();
      }
    }
  } else if (node.has_val()) {
    auto candidate = iterate_node(node);
    if (candidate.has_value()) {
      return candidate.value();
    }
  }
  return {};
}

std::expected<std::string, error> component_database::get_component_id(const path &path) const
{
  auto components_node = ryml_get_child(database.crootref(), "components");
  if (!components_node.valid() || !components_node.is_map()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  for (const auto &entry : components_node.children()) {
    if (!entry.has_key()) {
      continue;
    }
    std::string name;
    c4::from_chars(entry.key(), &name);
    if (entry.has_val() && std::filesystem::path{ ryml_val_string(entry) } == path) {
      return name;
    }
    if (entry.is_seq()) {
      for (const auto &n : entry.children()) {
        if (std::filesystem::path{ ryml_val_string(n) } == path) {
          return name;
        }
      }
    }
  }
  return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

std::optional<const json> component_database::get_blueprint_provider(std::string_view blueprint) const
{
  const auto blueprint_str = std::string{ blueprint };
  auto blueprints_node = ryml_get_child(database.crootref(), "blueprints");
  if (blueprints_node.valid() && blueprints_node.has_child(c4::to_csubstr(blueprint_str))) {
    return std::optional<const json>(std::in_place, ryml_to_json(ryml_get_child(blueprints_node, c4::to_csubstr(blueprint_str))));
  }

  return std::nullopt;
}

std::optional<const json> component_database::get_serve_endpoint_provider(std::string_view endpoint) const
{
  const auto endpoint_str = std::string{ endpoint };
  auto serve_node = ryml_get_child(database.crootref(), "serve");
  if (serve_node.valid() && serve_node.has_child(c4::to_csubstr(endpoint_str))) {
    return std::optional<const json>(std::in_place, ryml_to_json(ryml_get_child(serve_node, c4::to_csubstr(endpoint_str))));
  }
  return std::nullopt;
}

std::expected<void, std::error_code> component_database::parse_yakka_file(const path &path, std::string_view id)
{
  auto result = yakka::get_file_contents<std::vector<char>>(path.string());
  if (!result) {
    return std::unexpected(result.error());
  }
  ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(*result));
  auto root       = tree.crootref();

  // Check for blueprints and process them
  if (root.has_child("blueprints")) {
    for (const auto &b: root["blueprints"].children()) {
      process_blueprint(database, id, b);
    }
  }

  // Check for Yakka serve endpoints
  if (root.has_child("yakka") && root["yakka"].has_child("serve")) {
    for (const auto &f: root["yakka"]["serve"].children()) {
      std::string endpoint = std::string{ f.val().str, f.val().len };
      auto serve_node = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "serve", endpoint }, true);
      if (!serve_node.is_seq()) {
        serve_node |= ryml::SEQ;
      }
      auto entry = serve_node.append_child();
      entry.set_val(c4::to_csubstr(std::string{ id }));
    }
  }

  if (root.has_child("type")) {
    if (root["type"].is_seq()) {
      for (const auto &t: root["type"].children()) {
        std::string type_name = std::string{ t.val().str, t.val().len };
        auto type_node = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "types", type_name }, true);
        if (!type_node.is_seq()) {
          type_node |= ryml::SEQ;
        }
        auto entry = type_node.append_child();
        entry.set_val(c4::to_csubstr(std::string{ id }));
      }
    } else {
      std::string type_name = std::string{ root["type"].val().str, root["type"].val().len };
      auto type_node = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "types", type_name }, true);
      if (!type_node.is_seq()) {
        type_node |= ryml::SEQ;
      }
      auto entry = type_node.append_child();
      entry.set_val(c4::to_csubstr(std::string{ id }));
    }
  }

  return {};
}

std::optional<const json> component_database::get_feature_provider(std::string_view feature) const
{
  const auto feature_str = std::string{ feature };
  auto features_node = ryml_get_child(database.crootref(), "features");
  if (features_node.valid() && features_node.has_child(c4::to_csubstr(feature_str))) {
    return std::optional<const json>(std::in_place, ryml_to_json(ryml_get_child(features_node, c4::to_csubstr(feature_str))));
  }
  return std::nullopt;
}

std::expected<void, std::error_code> component_database::parse_slcc_file(const path &path)
{
  try {
    auto file_content = yakka::get_file_contents<std::vector<char>>(path.string());
    if (!file_content) {
      return std::unexpected(file_content.error());
    }
    ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(*file_content));
    auto root       = tree.crootref();

    c4::yml::ConstNodeRef id_node;
    c4::yml::ConstNodeRef provides_node;
    c4::yml::ConstNodeRef blueprint_node;

    // Check if the slcc is an omap
    if (root.is_seq()) {
      for (const auto &c: root.children()) {
        if (c.has_child("id"))
          id_node = c["id"];
        if (c.has_child("provides"))
          provides_node = c["provides"];
        if (c.has_child("blueprints"))
          blueprint_node = c["blueprints"];
      }
    } else {
      if (root.has_child("id"))
        id_node = root["id"];
      if (root.has_child("provides"))
        provides_node = root["provides"];
      if (root.has_child("blueprints"))
        blueprint_node = root["blueprints"];
    }

    if (!id_node.valid()) {
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    std::string id_string = std::string(id_node.val().str, id_node.val().len);

    auto result = add_component(id_string, path);
    if (!result) {
      return std::unexpected(result.error());
    } else if (*result) {
      if (provides_node.valid()) {
        for (const auto &f: provides_node.children()) {
          if (!f.has_child("name"))
            continue;
          auto feature_node        = f["name"].val();
          std::string feature_name = std::string(feature_node.str, feature_node.len);
          if (f.has_child("condition")) {
            ryml::Tree node;
            auto node_root = node.rootref();
            node_root |= ryml::MAP;
            auto name_child = node_root.append_child();
            name_child << ryml::key("name");
            name_child.set_val(c4::to_csubstr(id_string));
            auto condition_child = node_root.append_child();
            condition_child << ryml::key("condition");
            condition_child |= ryml::SEQ;
            for (const auto &c: f["condition"].children()) {
              std::string condition_string = std::string(c.val().str, c.val().len);
              auto cond_entry = condition_child.append_child();
              cond_entry.set_val(c4::to_csubstr(condition_string));
            }
            auto feature_node_ref = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "features", feature_name }, true);
            if (!feature_node_ref.is_seq()) {
              feature_node_ref |= ryml::SEQ;
            }
            auto new_entry = feature_node_ref.append_child();
            new_entry |= ryml::MAP;
            new_entry.tree()->merge_with(&node, node.root_id(), new_entry.id());
          } else {
            auto feature_node_ref = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "features", feature_name }, true);
            if (!feature_node_ref.is_seq()) {
              feature_node_ref |= ryml::SEQ;
            }
            auto new_entry = feature_node_ref.append_child();
            new_entry.set_val(c4::to_csubstr(id_string));
          }
        }
      }

      if (blueprint_node.valid()) {
        for (const auto &b: blueprint_node.children()) {
          process_blueprint(database, id_string, b);
        }
      }
    }

    return {};
  } catch (const std::exception &) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
}

static void process_blueprint(ryml::Tree &database, std::string_view id_string, const c4::yml::ConstNodeRef &blueprint_node)
{
  // Ignore regex blueprints
  if (blueprint_node.has_child("regex")) {
    return;
  }
  // Ignore templated blueprints
  if (blueprint_node.key().find("{") != ryml::npos) {
    return;
  }

  // Store blueprint entry
  std::string blueprint_name = std::string(blueprint_node.key().str, blueprint_node.key().len);
  spdlog::info("Found blueprint: {}", blueprint_name);
  auto blueprint_node_ref = ryml_navigate_path(database.rootref(), std::vector<std::string>{ "blueprints", blueprint_name }, true);
  if (!blueprint_node_ref.is_seq()) {
    blueprint_node_ref |= ryml::SEQ;
  }
  auto entry = blueprint_node_ref.append_child();
  entry.set_val(c4::to_csubstr(std::string{ id_string }));
}

} // namespace yakka