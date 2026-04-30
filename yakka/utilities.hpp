#pragma once

#include "yaml-cpp/yaml.h"
#include "inja.hpp"
#include "pugixml.hpp"
#include "yakka_schema.hpp"
// #include "rapidyaml_pointer.hpp"
// #include "pointer.hpp"

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

// #include <fmt/format.h>
// #include <c4/substr.hpp>
template<>
struct fmt::formatter<c4::csubstr> : fmt::formatter<std::string_view>
{
    template<typename FormatContext>
    auto format(const c4::csubstr& s, FormatContext& ctx)
    {
        return fmt::formatter<std::string_view>::format(
            std::string_view{s.str, s.len},
            ctx
        );
    }
};


namespace yakka {

void ryml_node_merge(ryml::ConstNodeRef source, ryml::NodeRef target, const schema* schema = nullptr);
// void json_node_merge(const std::vector<std::string> &path, ryml::NodeRef merge_target, ryml::ConstNodeRef node, const schema* schema = nullptr);
void json_node_merge(ryml::Pointer path, ryml::NodeRef merge_target, ryml::ConstNodeRef node, const schema* schema = nullptr);
void merge_nodes(ryml::NodeRef dst, ryml::ConstNodeRef src);

std::pair<std::string, int> exec(const std::string &command_text, const std::string &arg_text);
int exec(const std::string &command_text, const std::string &arg_text, std::function<void(std::string &)> function);
bool yaml_diff(const YAML::Node &node1, const YAML::Node &node2);
YAML::Node yaml_path(const YAML::Node &node, std::string path);

bool ryml_has_child(ryml::ConstNodeRef node, c4::csubstr key);
std::expected<ryml::Tree, std::error_code> ryml_load_file(const std::filesystem::path &path);
void ryml_save_file(const std::filesystem::path &path, ryml::ConstNodeRef node);
std::filesystem::path ryml_path(c4::csubstr path);
static inline std::string ryml_string(c4::csubstr str)
{
  return std::string(str.data(), str.size());
}

// std::tuple<component_list_t, feature_list_t, command_list_t> parse_arguments(const std::vector<std::string> &argument_string);
std::string generate_project_name(const component_list_t &components, const feature_list_t &features);
std::vector<ryml::csubstr> parse_gcc_dependency_file(const std::string &filename);
ryml::csubstr component_dotname_to_id(const ryml::csubstr dotname);
std::filesystem::path get_yakka_shared_home();
std::string try_render(inja::Environment &env, std::string_view input, ryml::ConstNodeRef data);
std::string try_render(inja::Environment &env, ryml::csubstr input, ryml::ConstNodeRef data);
std::string try_render(inja::Environment &env, const std::string &input, ryml::ConstNodeRef data);
std::string try_render(inja::Environment &env, ryml::ConstNodeRef input, ryml::ConstNodeRef data);
std::string try_render_file(inja::Environment &env, const std::string &filename, ryml::ConstNodeRef data);
std::string try_render_file(inja::Environment &env, const std::filesystem::path &file_path, ryml::ConstNodeRef data);
std::pair<std::string, int> download_resource(const std::string url, std::filesystem::path destination);
ryml::Pointer create_condition_pointer(ryml::ConstNodeRef condition);
void find_json_keys(ryml::ConstNodeRef j, const std::string &target_key, const std::string &current_path, ryml::NodeRef paths);

void hash_file(std::filesystem::path filename, uint8_t out_hash[32]) noexcept;
void xml_to_json(const pugi::xml_node& node, ryml::NodeRef& target);

std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, ryml::ConstNodeRef left, ryml::ConstNodeRef right) noexcept;

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