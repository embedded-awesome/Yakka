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

static void process_blueprint(ryml::Tree &database, ryml::csubstr id_string, const c4::yml::ConstNodeRef blueprint_node);
namespace fs = std::filesystem;
using error  = std::error_code;

static void initialize_database(ryml::Tree &database)
{
  // database.clear();
  auto root = database.rootref();
  root |= ryml::MAP;

  auto ensure_map = [&](const char *key) {
    ryml::NodeRef child = root.append_child();
    child << ryml::Key(key);
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
  initialize_database(database);
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

void component_database::insert(ryml::csubstr id, const path &config_file)
{
  auto pointer         = ryml::Pointer{ ryml::csubstr{ "components" } } / id;
  auto components_node = database.rootref()[pointer]; //ryml_navigate_path(database.rootref(), std::vector<std::string>{ "components", std::string{ id } }, true);
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
      // initialize_database(database);
      scan_for_components(this->workspace_path);
      return save();
    } else {
      auto loaded = ryml_load_file(database_filename);
      if (!loaded) {
        // initialize_database(database);
        scan_for_components(this->workspace_path);
        return save();
      }
      database = std::move(*loaded);
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
  initialize_database(database);
  database_is_dirty = true;
}

[[nodiscard]] std::expected<bool, std::error_code> component_database::add_component(std::string component_id, const path &path)
{
  return add_component(c4::to_csubstr(component_id), path);
}

std::expected<bool, error> component_database::add_component(ryml::csubstr component_id, const path &path)
{
  auto abs_path = fs::absolute(path);

  if (!fs::exists(abs_path)) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const auto path_string = abs_path.generic_string();

  auto components_node = database["components"];
  if (components_node.has_child(component_id)) {
    auto entries = components_node[component_id];
    if (entries.is_seq()) {
      for (auto entry: entries.children()) {
        if (entry.val<std::string>().value() == path_string) {
          return false;
        }
      }
    } else if (entries.has_val() && entries.val<std::string>().value() == path_string) {
      return false;
    }
  }

  auto components = database["components"];
  auto entry_node = components.append_child() << ryml::key(component_id);
  entry_node |= ryml::SEQ;
  entry_node.append_child() << path_string;
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
    const auto &path     = entry.path();
    const auto ext       = path.extension();
    const auto id        = path.stem().string();
    const auto id_substr = c4::to_csubstr(id);

    if (auto result = add_component(id, path); result && *result) {
      if (ext == yakka_component_extension || ext == yakka_component_old_extension) {
        parse_yakka_file(path, id_substr);
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

std::expected<path, std::error_code> component_database::get_component(const std::string &id, flag flags) const
{
  return get_component(c4::to_csubstr(id), flags);
}

std::expected<path, error> component_database::get_component(ryml::csubstr id, flag flags) const
{
  auto components_node = database["components"];
  if (!components_node.has_child(id)) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  auto node   = components_node[id];
  auto iterate_node = [&](const ryml::ConstNodeRef &entry) -> std::optional<path> {
    const auto path      = this->workspace_path / std::filesystem::path{ entry.val<std::string>().value() };
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
    for (const auto &n: node.children()) {
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
  auto components_node = database["components"];

  for (const auto &entry: components_node.children()) {
    if (!entry.has_key()) {
      continue;
    }
    std::string name;
    c4::from_chars(entry.key(), &name);
    if (entry.has_val() && std::filesystem::path{ entry.val<std::string>().value() } == path) {
      return name;
    }
    if (entry.is_seq()) {
      for (const auto &n: entry.children()) {
        if (std::filesystem::path{ n.val<std::string>().value() } == path) {
          return name;
        }
      }
    }
  }
  return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

std::optional<ryml::ConstNodeRef> component_database::get_blueprint_provider(ryml::csubstr blueprint) const
{
  // const auto blueprint_str = std::string{ blueprint };
  auto blueprints_node = database["blueprints"];
  if (blueprints_node.has_child(blueprint)) {
    return blueprints_node[blueprint];
  }

  return std::nullopt;
}

std::optional<ryml::ConstNodeRef> component_database::get_serve_endpoint_provider(ryml::csubstr endpoint) const
{
  // const auto endpoint_str = std::string{ endpoint };
  if (database["serve"].has_child(endpoint)) {
    return database["serve"][endpoint];
  }
  return std::nullopt;
}

std::expected<void, std::error_code> component_database::parse_yakka_file(const path &path, ryml::csubstr id)
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
      auto serve_node = database["serve"].append_child();
      serve_node |= ryml::SEQ;
      serve_node.set_key_serialized(f.val()); // This is done as f.val() is from another tree
      serve_node.append_child() << id;
    }
  }

  if (root.has_child("type")) {
    if (root["type"].is_seq()) {
      for (const auto &t: root["type"].children()) {
        auto type_node = database["types"].append_child();
        type_node |= ryml::SEQ;
        type_node.set_key_serialized(t.val());
        type_node.append_child() << id;
      }
    } else {
      auto type_node = database["types"].append_child();
      type_node |= ryml::SEQ;
      type_node.set_key_serialized(root["type"].val());
      type_node.append_child() << id;
    }
  }

  return {};
}

std::optional<ryml::ConstNodeRef> component_database::get_feature_provider(ryml::csubstr feature) const
{
  // const auto feature_str = std::string{ feature };
  auto features_node = database.crootref()["features"];
  if (features_node.valid() && features_node[feature].valid()) {
    return features_node[feature];
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

    auto result = add_component(id_node.val(), path);
    if (!result) {
      return std::unexpected(result.error());
    } else if (*result) {
      if (provides_node.valid()) {
        for (const auto &f: provides_node.children()) {
          if (!f.has_child("name"))
            continue;
          // Create new feature node
          auto feature_node_ref = database["features"].append_child();
          feature_node_ref |= ryml::SEQ;
          feature_node_ref.set_key_serialized(f["name"].val());

          if (f.has_child("condition")) {
            auto new_entry = feature_node_ref.append_child();
            new_entry |= ryml::MAP;

            auto name_child = new_entry.append_child() << ryml::key("name");
            name_child.set_val_serialized(id_node.val());
            auto condition_child = new_entry.append_child() << ryml::key("condition");
            condition_child |= ryml::SEQ;
            for (const auto &c: f["condition"].children()) {
              auto node = condition_child.append_child();
              node.set_val_serialized(c.val());
            }
          } else {
            auto new_entry = feature_node_ref.append_child();
            new_entry.set_val_serialized(id_node.val());
          }
        }
      }

      if (blueprint_node.valid()) {
        for (const auto &b: blueprint_node.children()) {
          // TODO. This is passing a csubstr and ConstNodeRef from another tree. This needs to be copied into arena
          process_blueprint(database, id_node.val(), b);
        }
      }
    }

    return {};
  } catch (const std::exception &) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
}

static void process_blueprint(ryml::Tree &database, ryml::csubstr id_string, const c4::yml::ConstNodeRef blueprint_node)
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
  spdlog::info("Found blueprint: {}", blueprint_node.key());
  auto blueprint_node_ref = database["blueprints"].append_child();
  blueprint_node_ref |= ryml::SEQ;
  blueprint_node_ref.set_key_serialized(blueprint_node.key()); // ryml_navigate_path(database.rootref(), std::vector<std::string>{ "blueprints", blueprint_name }, true);
  auto entry = blueprint_node_ref.append_child();
  entry.set_val_serialized(id_string);
}

} // namespace yakka