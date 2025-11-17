#pragma once

#include "yaml-cpp/yaml.h"
#include "inja.hpp"
#include "yakka_schema.hpp"
#include <string>
#include <string_view>
#include <expected>
#include <unordered_set>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace yakka {
std::pair<std::string, int> exec(const std::string &command_text, const std::string &arg_text);
int exec(const std::string &command_text, const std::string &arg_text, std::function<void(std::string &)> function);
bool yaml_diff(const YAML::Node &node1, const YAML::Node &node2);
void json_node_merge(nlohmann::json::json_pointer path, nlohmann::json &merge_target, const nlohmann::json &node, const schema* schema = nullptr);
YAML::Node yaml_path(const YAML::Node &node, std::string path);
nlohmann::json json_path(const nlohmann::json &node, std::string path);
// nlohmann::json::json_pointer json_pointer(std::string path);
std::tuple<component_list_t, feature_list_t, command_list_t> parse_arguments(const std::vector<std::string> &argument_string);
std::string generate_project_name(const component_list_t &components, const feature_list_t &features);
std::vector<std::string> parse_gcc_dependency_file(const std::string &filename);
std::string component_dotname_to_id(const std::string dotname);
std::filesystem::path get_yakka_shared_home();
std::string try_render(inja::Environment &env, const std::string &input, const nlohmann::json &data);
std::string try_render_file(inja::Environment &env, const std::string &filename, const nlohmann::json &data);
std::pair<std::string, int> download_resource(const std::string url, std::filesystem::path destination);
nlohmann::json::json_pointer create_condition_pointer(const nlohmann::json condition);
void find_json_keys(const nlohmann::json &j, const std::string &target_key, const std::string &current_path, nlohmann::json& paths);

void hash_file(std::filesystem::path filename, uint8_t out_hash[32]) noexcept;

std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, const nlohmann::json &left, const nlohmann::json &right) noexcept;

void add_common_template_commands(inja::Environment &inja_env);

template <class CharContainer>
static std::expected<size_t, std::error_code> get_file_contents(std::filesystem::path filename, CharContainer *container)
{
  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file) {
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const auto file_size = file.tellg();
  if (file_size < 0) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }

  container->resize(static_cast<typename CharContainer::size_type>(file_size));

  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(container->data()), file_size)) {
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }

  return container->size();
}

template <class CharContainer>
static std::expected<CharContainer, std::error_code> get_file_contents(std::filesystem::path filename)
{
  CharContainer cc;
  auto result = get_file_contents(filename, &cc);
  if (result) {
    return cc;
  }
  return std::unexpected(result.error());
}

} // namespace yakka