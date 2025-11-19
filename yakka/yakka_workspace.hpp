#pragma once

#include "yaml-cpp/yaml.h"
#include "spdlog/spdlog.h"
#include "inja.hpp"
#include "component_database.hpp"
#include <string>
#include <future>
#include <expected>
#include <optional>
#include <filesystem>
#include <functional>
#include <system_error>

namespace fs = std::filesystem;

namespace yakka {
/**
 * @brief Manages a Yakka workspace including components, registries, and project configuration
 */
class workspace {
public:
  /** @brief Default constructor */
  workspace() = default;
  /** @brief Default destructor */
  ~workspace() = default;

  /**
   * @brief Initializes the workspace with given path
   * @param workspace_path Path to initialize workspace at, defaults to current path
   * @return std::expected<void, std::error_code> Success or error code
   * 
   * Initializes workspace by:
   * - Loading config file
   * - Setting up shared component paths
   * - Creating necessary directories
   * - Loading local and shared component databases
   * - Loading package registries
   * - Setting up project configuration
   */
  std::expected<void, std::error_code> init(const std::filesystem::path &workspace_path = fs::current_path());

  /**
   * @brief Asynchronously fetches a component from a remote source
   * @param name Name of the component to fetch
   * @param node YAML node containing component configuration
   * @param progress_handler Function to report progress during fetch
   * @return Future containing the path where component was fetched
   */
  std::future<std::filesystem::path> fetch_component(std::string_view name, const YAML::Node &node, std::function<void(std::string_view, size_t)> progress_handler);

  /**
   * @brief Loads all component registries from the workspace
   * 
   * Scans the .yakka/registries directory for registry files and loads them into the registries map
   */
  void load_component_registries();

  /**
   * @brief Adds a new component registry from a URL
   * @param url URL of the registry to add
   * @return Success or error code
   * 
   * The registry is cloned under the .yakka/registries directory 
   */
  std::expected<void, std::error_code> add_component_registry(std::string_view url);

  /**
   * @brief Finds a component in the loaded registries
   * @param name Name of the component to find
   * @return Optional YAML node containing the component details if found
   */
  std::optional<YAML::Node> find_registry_component(std::string_view name) const;

  /**
   * @brief Finds a component in local, shared or package databases
   * @param component_dotname Dot-notation name of component
   * @param flags Search flags for component type filtering. Defaults to matching all types.
   * @return Optional pair of component path and its containing path
   * 
   * The search order is local database, shared database, then package databases
   * The flags parameter can be used to filter component types (e.g., only SLCC, ignore Yakka components, etc.)
   */
  std::optional<std::pair<std::filesystem::path, std::filesystem::path>> find_component(std::string_view component_dotname, component_database::flag flags = component_database::flag::ALL_COMPONENTS);

  /**
   * @brief Finds a feature provider in the workspace
   * @param feature Name of the feature to find
   * @return Optional JSON containing feature provider info
   */
  std::optional<nlohmann::json> find_feature(std::string_view feature) const;

  /**
   * @brief Finds a blueprint provider in the workspace
   * @param blueprint Name of the blueprint to find
   * @return Optional JSON containing blueprint provider info
   */
  std::optional<nlohmann::json> find_blueprint(std::string_view blueprint) const;

  /**
   * @brief Loads the workspace configuration file
   * @param config_file_path Path to the config file
   * @return Success or error code
   */
  std::expected<void, std::error_code> load_config_file(const std::filesystem::path &config_file_path);

  /**
   * @brief Renders a template string using the workspace's template environment
   * @param input Template string to render
   * @return Rendered string
   */
  std::string template_render(const std::string input);

  /**
   * @brief Fetches a registry from a remote URL
   * @param url URL of the registry to fetch
   * @return Success or error code
   */
  std::expected<void, std::error_code> fetch_registry(std::string_view url);

  /**
   * @brief Updates a component to its latest version
   * @param name Name of the component to update
   * @return Success or error code
   */
  std::expected<void, std::error_code> update_component(std::string_view name);

  /**
   * @brief Gets the path to the Yakka shared home directory
   * @return Path to the shared home directory
   */
  std::filesystem::path get_yakka_shared_home();

  /**
   * @brief Executes a Git command in the specified directory
   * @param command Git command to execute
   * @param git_directory_string Directory where to execute the command
   * @return Success or error code
   */
  std::expected<void, std::error_code> execute_git_command(std::string_view command, std::string_view git_directory_string);

  /**
   * @brief Performs the actual component fetching operation
   * @param name Component name
   * @param url Source URL
   * @param branch Git branch to fetch
   * @param git_location Location for git operations
   * @param checkout_location Where to checkout the component
   * @param progress_handler Function to report progress
   * @return Path where component was fetched or error code
   */
  static std::expected<std::filesystem::path, std::error_code> do_fetch_component(std::string_view name,
                                                                     std::string_view url,
                                                                     std::string_view branch,
                                                                     const std::filesystem::path &git_location,
                                                                     const std::filesystem::path &checkout_location,
                                                                     std::function<void(std::string_view, size_t)> progress_handler);

  /**
   * @brief Updates the workspace version information
   */
  void update_versions();

  /**
   * @brief Finds all component registry files in the workspace
   */
  std::vector<std::filesystem::path> find_component_registries() const;

public:
  /** @brief Logger instance for workspace operations */
  std::shared_ptr<spdlog::logger> log;

  /** @brief Collection of loaded component registries */
  YAML::Node registries;

  /** @brief Summary information about the workspace */
  nlohmann::json summary;

  /** @brief Projects configuration and information */
  nlohmann::json projects;

  /** @brief Workspace version information */
  nlohmann::json versions;

  /** @brief List of ongoing component fetch operations */
  std::map<std::string, std::future<void>> fetching_list;

  /** @brief Path to the current workspace root */
  std::filesystem::path workspace_path;

  /** @brief Path to shared components directory */
  std::filesystem::path shared_components_path;

  /** @brief Template engine environment for rendering */
  inja::Environment inja_environment;

  /** @brief Database of locally available components */
  component_database local_database;

  /** @brief Database of shared components */
  component_database shared_database;

  /** @brief Path to Yakka's shared home directory */
  std::filesystem::path yakka_shared_home;

  /** @brief List of package paths to search */
  std::vector<std::filesystem::path> packages;

  /** @brief Collection of component databases from package paths */
  std::vector<component_database> package_databases;
};
} // namespace yakka
