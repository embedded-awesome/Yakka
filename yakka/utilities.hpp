#pragma once

#include "yaml-cpp/yaml.h"
#include "inja.hpp"
#include <string>
#include <string_view>
#include <expected>
#include <unordered_set>
#include <filesystem>

namespace fs = std::filesystem;

namespace yakka {
using component_list_t = std::unordered_set<std::string>;
using feature_list_t   = std::unordered_set<std::string>;
using command_list_t   = std::unordered_set<std::string>;

std::pair<std::string, int> exec(const std::string &command_text, const std::string &arg_text);
int exec(const std::string &command_text, const std::string &arg_text, std::function<void(std::string &)> function);
bool yaml_diff(const YAML::Node &node1, const YAML::Node &node2);
void json_node_merge(nlohmann::json &merge_target, const nlohmann::json &node);
YAML::Node yaml_path(const YAML::Node &node, std::string path);
nlohmann::json json_path(const nlohmann::json &node, std::string path);
nlohmann::json::json_pointer json_pointer(std::string path);
std::tuple<component_list_t, feature_list_t, command_list_t> parse_arguments(const std::vector<std::string> &argument_string);
std::string generate_project_name(const component_list_t &components, const feature_list_t &features);
std::vector<std::string> parse_gcc_dependency_file(const std::string &filename);
std::string component_dotname_to_id(const std::string dotname);
fs::path get_yakka_shared_home();
std::string try_render(inja::Environment &env, const std::string &input, const nlohmann::json &data);
std::string try_render_file(inja::Environment &env, const std::string &filename, const nlohmann::json &data);
std::pair<std::string, int> download_resource(const std::string url, fs::path destination);
nlohmann::json::json_pointer create_condition_pointer(const nlohmann::json condition);

std::expected<bool, std::string> has_data_dependency_changed(
    std::string_view data_path,
    const nlohmann::json& left,
    const nlohmann::json& right) noexcept;
    
void add_common_template_commands(inja::Environment &inja_env);

template <class CharContainer> static size_t get_file_contents(const std::string &filename, CharContainer *container)
{
  ::FILE *file = ::fopen(filename.c_str(), "rb");
  if (file == nullptr) {
    return 0;
  }
  ::fseek(file, 0, SEEK_END);
  long size = ::ftell(file);
  container->resize(static_cast<typename CharContainer::size_type>(size));
  if (size) {
    ::rewind(file);
    size_t ret = ::fread(&(*container)[0], 1, container->size(), file);
    (void)ret;
    //C4_CHECK(ret == (size_t)size);
  }
  ::fclose(file);
  return container->size();
}

template <class CharContainer> CharContainer get_file_contents(const std::string &filename)
{
  CharContainer cc;
  get_file_contents(filename, &cc);
  return cc;
}

} // namespace yakka