#pragma once

#include "yaml-cpp/yaml.h"
#include "inja.hpp"
#include "pugixml.hpp"
#include "yakka_schema.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <string>
#include <string_view>
#include <expected>
#include <unordered_set>
#include <filesystem>
#include <optional>
#include <vector>

namespace fs = std::filesystem;

namespace yakka {
struct RymlPointer {
  RymlPointer() = default;
  explicit RymlPointer(std::string path);
  explicit RymlPointer(std::vector<std::string> segments_in);

  const std::vector<std::string> &parts() const noexcept;
  bool empty() const noexcept;

private:
  std::vector<std::string> segments;
};

RymlPointer operator/(RymlPointer lhs, const RymlPointer &rhs);
RymlPointer &operator/=(RymlPointer &lhs, const RymlPointer &rhs);
RymlPointer operator/(RymlPointer lhs, const std::string &segment);
RymlPointer &operator/=(RymlPointer &lhs, const std::string &segment);
RymlPointer operator/(RymlPointer lhs, size_t index);
RymlPointer &operator/=(RymlPointer &lhs, size_t index);

std::pair<std::string, int> exec(const std::string &command_text, const std::string &arg_text);
int exec(const std::string &command_text, const std::string &arg_text, std::function<void(std::string &)> function);
bool yaml_diff(const YAML::Node &node1, const YAML::Node &node2);
void json_node_merge(RymlPointer path, ryml::Tree &merge_target, const ryml::Tree &node, const schema* schema = nullptr);
YAML::Node yaml_path(const YAML::Node &node, std::string path);
ryml::Tree json_path(const ryml::Tree &node, std::string path);
// RymlPointer ryml_pointer(std::string path);
RymlPointer ryml_pointer(std::string path);
std::tuple<component_list_t, feature_list_t, command_list_t> parse_arguments(const std::vector<std::string> &argument_string);
std::string generate_project_name(const component_list_t &components, const feature_list_t &features);
std::vector<std::string> parse_gcc_dependency_file(const std::string &filename);
std::string component_dotname_to_id(const std::string dotname);
std::filesystem::path get_yakka_shared_home();
std::string try_render(inja::Environment &env, const std::string &input, const ryml::Tree &data);
std::string try_render_file(inja::Environment &env, const std::string &filename, const ryml::Tree &data);
std::string try_render(inja::Environment &env, const std::string &input, const ryml::ConstNodeRef &data);
std::string try_render_file(inja::Environment &env, const std::string &filename, const ryml::ConstNodeRef &data);
std::pair<std::string, int> download_resource(const std::string url, std::filesystem::path destination);
RymlPointer create_condition_pointer(const ryml::Tree condition);
void find_json_keys(const inja::json &j, const std::string &target_key, const std::string &current_path, inja::json &paths);

void hash_file(std::filesystem::path filename, uint8_t out_hash[32]) noexcept;
ryml::Tree xml_to_json(const pugi::xml_node& node);

std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, const ryml::Tree &left, const ryml::Tree &right) noexcept;

void add_common_template_commands(inja::Environment &inja_env);

// RapidYAML conversion utilities
ryml::Tree ryml_to_json(const ryml::ConstNodeRef &node);
inja::json ryml_to_inja_json(const ryml::ConstNodeRef &node);
void ryml_node_merge(const ryml::ConstNodeRef &source, ryml::NodeRef target, const schema* schema = nullptr);
void json_node_merge(const std::vector<std::string> &path, ryml::NodeRef merge_target, const ryml::ConstNodeRef &node, const schema* schema = nullptr);
std::string ryml_get_val_as_string(const ryml::ConstNodeRef &node);
bool ryml_has_child(const ryml::ConstNodeRef &node, c4::csubstr key);
ryml::ConstNodeRef ryml_get_child(const ryml::ConstNodeRef &node, c4::csubstr key);
bool ryml_has_path(const ryml::ConstNodeRef &node, const RymlPointer &path);
ryml::ConstNodeRef ryml_get_path(const ryml::ConstNodeRef &node, const RymlPointer &path);
ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const std::vector<std::string> &path, bool create_if_missing = true);
ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const RymlPointer &path, bool create_if_missing = true);
std::expected<ryml::Tree, std::error_code> ryml_load_file(const std::filesystem::path &path);

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