/**
 * @file utilities.cpp
 * @brief Implements shared utility helpers used across Yakka subsystems.
 */

#include "yakka_project.hpp"
#include "utilities.hpp"
#include "subprocess.hpp"
#include "spdlog/spdlog.h"

#include "glob/glob.h"
#include "yakka_schema.hpp"
#include "blake3.h"
#include "pugixml.hpp"
#include "rapidyaml_json.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <concepts>
#include <string_view>
#include <expected>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace yakka {

/*
// Execute with admin
if(0 == CreateProcess(argv[2], params, NULL, NULL, false, 0, NULL, NULL, &si, &pi)) {
        //runas word is a hack to require UAC elevation
        ShellExecute(NULL, "runas", argv[2], params, NULL, SW_SHOWNORMAL);
}
/// @brief Executes exec.

*/
std::pair<std::string, int> exec(const std::string &command_text, const std::string &arg_text)
{

  spdlog::info("{} {}", command_text, arg_text);
  try {
    std::string command = command_text;
    if (!arg_text.empty())
      command += " " + arg_text;
#if defined(__USING_WINDOWS__)
    auto p = subprocess::Popen(command, subprocess::output{ subprocess::PIPE }, subprocess::error{ subprocess::STDOUT });
#else
    auto p = subprocess::Popen(command, subprocess::shell{ true }, subprocess::output{ subprocess::PIPE }, subprocess::error{ subprocess::STDOUT });
#endif
#if defined(__USING_WINDOWS__)
    auto output  = p.communicate().first;
    auto retcode = p.wait();
    retcode      = p.poll();
#else
    auto output = p.communicate().first;
    p.wait();
    auto retcode = p.retcode();
#endif
    std::string output_text = output.buf.data();
    return { output_text, retcode };
  } catch (std::exception &e) {
    spdlog::error("Exception while executing: {}\n{}", command_text, e.what());
    return { "", -1 };
  }
}

/// @brief Executes exec.

int exec(const std::string &command_text, const std::string &arg_text, std::function<void(std::string &)> function)
{
  spdlog::info("{} {}", command_text, arg_text);

  try {
    std::string command = command_text;
    if (!arg_text.empty())
      command += " " + arg_text;
#if defined(__USING_WINDOWS__)
    auto p = subprocess::Popen(command, subprocess::output{ subprocess::PIPE }, subprocess::error{ subprocess::STDOUT });
#else
    auto p = subprocess::Popen(command, subprocess::shell{ true }, subprocess::output{ subprocess::PIPE }, subprocess::error{ subprocess::STDOUT });
#endif
    auto output = p.output();
    std::array<char, 512> buffer;
    size_t count = 0;
    buffer.fill('\0');
    if (output != nullptr) {
      while (1) {
        buffer[count] = fgetc(output);
        if (feof(output))
          break;

        if (count == buffer.size() - 1 || buffer[count] == '\r' || buffer[count] == '\n') {
          std::string temp(buffer.data());
          try {
            function(temp);
          } catch (std::exception &e) {
            spdlog::debug("exec() data processing threw exception '{}'for the following data:\n{}", e.what(), temp);
          }
          buffer.fill('\0');
          count = 0;
        } else
          ++count;
      };
    }
    auto retcode = p.wait();
#if defined(__USING_WINDOWS__)
    retcode = p.poll();
#endif
    return retcode;
  } catch (std::exception &e) {
    spdlog::error("Exception while executing: {}\n{}", command_text, e.what());
  }
  return -1;
}

/// @brief Executes yaml_diff.

bool yaml_diff(const YAML::Node &node1, const YAML::Node &node2)
{
  std::vector<std::pair<const YAML::Node &, const YAML::Node &>> compare_list;

  compare_list.push_back({ node1, node2 });
  for (size_t i = 0; i < compare_list.size(); ++i) {
    const YAML::Node &left  = compare_list[i].first;
    const YAML::Node &right = compare_list[i].second;

    if (left.Type() != right.Type())
      return true;

    switch (left.Type()) {
      case YAML::NodeType::Scalar:
        if (left.Scalar() != right.Scalar())
          return true;
        break;

      case YAML::NodeType::Sequence:
        // Verify the sequences have the same length
        if (left.size() != right.size())
          return true;
        for (size_t a = 0; a < left.size(); ++a)
          compare_list.push_back({ left[a], right[a] });
        break;

      case YAML::NodeType::Map:
        // Verify the maps have the same length
        if (left.size() != right.size())
          return true;
        for (const auto &a: left) {
          auto &key  = a.first.Scalar();
          auto &node = a.second;
          if (!right[key])
            return true;
          compare_list.push_back({ node, right[key] });
        }
        break;
      default:
        break;
    }
  }

  return false;
}

/// @brief Executes yaml_path.

YAML::Node yaml_path(const YAML::Node &node, std::string path)
{
  YAML::Node temp = node;

  std::stringstream ss(path);
  std::string s;
  while (std::getline(ss, s, '.'))
    temp.reset(temp[s]);
  return temp;
}

// static std::string unescape_ryml_pointer_segment(std::string_view segment)
// {
//   std::string result;
//   result.reserve(segment.size());
//   for (size_t i = 0; i < segment.size(); ++i) {
//     if (segment[i] == '~' && i + 1 < segment.size()) {
//       char next = segment[i + 1];
//       if (next == '0') {
//         result.push_back('~');
//         ++i;
//         continue;
//       }
//       if (next == '1') {
//         result.push_back('/');
//         ++i;
//         continue;
//       }
//     }
//     result.push_back(segment[i]);
//   }
//   return result;
// }

// static std::vector<std::string> parse_ryml_pointer_segments(std::string path)
// {
//   std::vector<std::string> segments;
//   if (path.empty()) {
//     return segments;
//   }

//   if (path.front() != '/') {
//     std::replace(path.begin(), path.end(), '.', '/');
//     path = "/" + path;
//   }

//   std::string_view path_view(path);
//   size_t start = 1;
//   while (start <= path_view.size()) {
//     size_t slash = path_view.find('/', start);
//     if (slash == std::string_view::npos) {
//       slash = path_view.size();
//     }
//     auto segment_view = path_view.substr(start, slash - start);
//     segments.emplace_back(unescape_ryml_pointer_segment(segment_view));
//     if (slash >= path_view.size()) {
//       break;
//     }
//     start = slash + 1;
//   }

//   return segments;
// }

/// @brief Executes generate_project_name.

std::string generate_project_name(const component_list_t &components, const feature_list_t &features)
{
  std::string project_name = "";


  // Generate the project name from the project string
  for (const auto &i: components)
    project_name += ryml_string(i) + "-";

  if (!components.empty())
    project_name.pop_back();

  for (const auto &i: features)
    project_name += "-" + ryml_string(i);

  if (project_name.empty())
    project_name = "none";

  return project_name;
}

/**
 * @brief Merges a node into a merge_target node according to the rules defined in the provided schema, if any.
 *       If no schema is provided, the node is merged according to the default rules: scalars are overwritten, sequences are concatenated, and maps are merged with new keys added and existing keys overwritten.
 * @param path  Path relative to Yakka component schema

/// @brief Executes copy_node_recursive.

 * @param merge_target  Node to merge into. This node is modified in place.
 * @param node  Node to merge. This node is not modified.
 * @param schema  Optional schema to define merge behavior. If not provided, default merge behavior is used.
 */
static void copy_node_recursive(ryml::ConstNodeRef source, ryml::NodeRef destination)
{
  if (source.is_map() || source.is_seq()) {
    // change_type() only removes children when the type actually changes.
    destination.clear_children();
    destination.change_type(source.is_map() ? ryml::MAP : ryml::SEQ);
    destination.tree()->duplicate_children(source.tree(), source.id(), destination.id(), c4::yml::NONE);
    return;
  }

  if (source.has_val()) {
    destination.change_type(ryml::VAL);
    destination.tree()->duplicate_contents(source.tree(), source.id(), destination.id());
  }
}

/// @brief Executes append_duplicate_children.

static void append_duplicate_children(ryml::ConstNodeRef source, ryml::NodeRef destination_parent)
{
  const auto after = destination_parent.num_children() > 0 ? destination_parent.last_child().id() : c4::yml::NONE;

  destination_parent.tree()->duplicate_children(source.tree(), source.id(), destination_parent.id(), after);
}

/// @brief Executes append_duplicate_node.

static void append_duplicate_node(ryml::ConstNodeRef source, ryml::NodeRef destination_parent)
{
  const auto after = destination_parent.num_children() > 0 ? destination_parent.last_child().id() : c4::yml::NONE;

  destination_parent.tree()->duplicate(source.tree(), source.id(), destination_parent.id(), after);
}

/// @brief Executes merge_nodes.

void merge_nodes(ryml::NodeRef dst, ryml::ConstNodeRef src)
{
  // If src is a map

  if (src.is_map()) {
    // dst.to_map(); // ensure type
    if (!dst.has_val() && !dst.is_seq())
      dst |= ryml::MAP;
    
    if (!dst.is_map()) {
      spdlog::error("Merging map into non-map node. Source: '{}', Destination: '{}'", src.key(), dst.key());
      return;
    }

    for (auto const &ch: src.children()) {
      auto key       = ch.key();
      auto dst_child = dst.has_child(key) ? dst.at(key) : dst.append_child();
      dst_child << ryml::key(key);

      merge_nodes(dst_child, ch);
    }
  }
  // If src is a sequence
  else if (src.is_seq()) {
    // dst.to_seq(); // ensure type
    if (!dst.has_val() && !dst.is_map())
      dst |= ryml::SEQ;
    if (!dst.is_seq()) {
      spdlog::error("Merging sequence into non-sequence node. Source: '{}', Destination: '{}'", src.key(), dst.key());
      return;
    }

    for (auto const &ch: src.children()) {
      auto dst_child = dst.append_child();
      merge_nodes(dst_child, ch);
    }
  }
  // Scalar
  else if (src.has_val()) {
    if (dst.is_seq()) {
      dst.append_child() << src.val();
    } else if (dst.is_map()) {
      spdlog::error("Merging scalar into map. Source: '{}', Destination: '{}'", src.key(), dst.key());
    } else {
      dst.set_val(src.val());
    }
  } else {
    spdlog::error("Merging node with unknown type. Source: '{}', Destination: '{}'", src.key(), dst.key());
  }
}

/// @brief Executes merge_node_default.

static void merge_node_default(ryml::NodeRef target, ryml::ConstNodeRef source)
{
  if (source.is_map()) {

    if (!target.is_map()) {
      copy_node_recursive(source, target);
      return;
    }

    for (const auto &child: source.children()) {
      if (!child.has_key()) {
        continue;
      }

      const auto key = child.key();
      if (!target.has_child(key)) {
        append_duplicate_node(child, target);
        continue;
      }

      auto target_child = target.find_child(key);
      if (child.is_map() && target_child.is_map()) {
        merge_node_default(target_child, child);
      } else if (child.is_seq() && target_child.is_seq()) {
        append_duplicate_children(child, target_child);
      } else {
        copy_node_recursive(child, target_child);
      }
    }
    return;
  }

  if (source.is_seq()) {
    if (!target.is_seq()) {
      copy_node_recursive(source, target);
      return;
    }

    append_duplicate_children(source, target);
    return;
  }

  copy_node_recursive(source, target);
}

/// @brief Executes json_node_merge.

static bool node_as_number(ryml::ConstNodeRef node, double &result)
{
  return node.has_val() && c4::from_chars(node.val(), &result);
}

/// @brief Removes duplicate scalar entries from a sequence, keeping the first occurrence.
static void deduplicate_seq(ryml::NodeRef seq)
{
  std::vector<std::string> seen;
  for (size_t i = 0; i < seq.num_children();) {
    auto child = seq.child(i);
    if (!child.has_val()) {
      ++i;
      continue;
    }

    std::string value = ryml_string(child.val());
    if (std::find(seen.begin(), seen.end(), value) != seen.end()) {
      seq.remove_child(child);
    } else {
      seen.push_back(std::move(value));
      ++i;
    }
  }
}

/// @brief Sorts a sequence of scalars by value.
static void sort_seq(ryml::NodeRef seq)
{
  std::vector<std::string> values;
  values.reserve(seq.num_children());
  for (const auto &child: seq.children()) {
    values.push_back(child.has_val() ? ryml_string(child.val()) : std::string{});
  }

  std::sort(values.begin(), values.end());

  size_t index = 0;
  for (auto child: seq.children()) {
    child << values[index++];
  }
}

static void merge_node_with_strategy(ryml::Pointer path, ryml::NodeRef target, ryml::ConstNodeRef source, const schema *schema);

/// @brief Merges each child of a source map into the target map, tracking the schema path.
static void merge_map_with_strategy(ryml::Pointer path, ryml::NodeRef target, ryml::ConstNodeRef source, const schema *schema)
{
  if (!target.is_map()) {
    copy_node_recursive(source, target);
    return;
  }

  for (const auto &child: source.children()) {
    if (!child.has_key()) {
      continue;
    }

    const auto key = child.key();
    if (!target.has_child(key)) {
      append_duplicate_node(child, target);
      continue;
    }

    merge_node_with_strategy(path / key, target.find_child(key), child, schema);
  }
}

static void merge_node_with_strategy(ryml::Pointer path, ryml::NodeRef target, ryml::ConstNodeRef source, const schema *schema)
{
  const auto strategy = schema == nullptr ? schema::merge_strategy::Default : schema->get_merge_strategy(path);

  switch (strategy) {
  case schema::merge_strategy::Overwrite:
    target.clear_children();
    copy_node_recursive(source, target);
    return;

  case schema::merge_strategy::Abort:
    if (ryml::emitrs_yaml<std::string>(target) != ryml::emitrs_yaml<std::string>(source)) {
      spdlog::error("Conflict detected while merging '{}'", path.to_string());
    }
    return;

  case schema::merge_strategy::Max:
  case schema::merge_strategy::Min: {
    double target_value = 0.0;
    double source_value = 0.0;
    if (!node_as_number(target, target_value) || !node_as_number(source, source_value)) {
      spdlog::error("Cannot apply numeric merge strategy at '{}': values are not numbers", path.to_string());
      return;
    }

    const bool take_source = strategy == schema::merge_strategy::Max ? source_value > target_value : source_value < target_value;
    if (take_source) {
      target.set_val(source.val());
    }
    return;
  }

  case schema::merge_strategy::Append:
  case schema::merge_strategy::Sort:
  case schema::merge_strategy::Unique: {
    if (!target.is_seq()) {
      if (target.has_val()) {
        const auto existing = target.val();
        target.clear_val();
        target |= ryml::SEQ;
        target.append_child() << existing;
      } else if (!target.is_map()) {
        target |= ryml::SEQ;
      }
    }

    if (target.is_seq()) {
      if (source.is_seq()) {
        append_duplicate_children(source, target);
      } else {
        // Sequence entries must not carry the source node's key.
        auto entry = target.append_child();
        copy_node_recursive(source, entry);
      }

      if (strategy == schema::merge_strategy::Unique) {
        deduplicate_seq(target);
      } else if (strategy == schema::merge_strategy::Sort) {
        sort_seq(target);
      }
      return;
    }

    [[fallthrough]];
  }

  case schema::merge_strategy::Default:
  default:
    break;
  }

  if (source.is_map()) {
    merge_map_with_strategy(std::move(path), target, source, schema);
    return;
  }

  if (source.is_seq()) {
    if (!target.is_seq()) {
      copy_node_recursive(source, target);
    } else {
      append_duplicate_children(source, target);
    }
    return;
  }

  // if (target.is_map() || target.is_seq()) {
  //   spdlog::warn("Merging scalar over container at '{}'", path.to_string());
  // }

  copy_node_recursive(source, target);
}

void json_node_merge(ryml::Pointer path, ryml::NodeRef merge_target, ryml::ConstNodeRef node, const schema *schema)
{
  if (!merge_target.valid() || !node.valid()) {
    return;
  }

  if (schema == nullptr) {
    merge_node_default(merge_target, node);
    return;
  }

  merge_node_with_strategy(std::move(path), merge_target, node, schema);
}

/// @brief Executes component_dotname_to_id.

ryml::csubstr component_dotname_to_id(const ryml::csubstr dotname)
{
  return dotname.find('.') != std::string::npos ? dotname.sub(dotname.rfind('.') + 1) : dotname;

}

/// @brief Executes try_render_file.

std::string try_render_file(inja::Environment &env, const std::string &filename, ryml::ConstNodeRef data)
{
  try {

    // auto json_data = ryml_to_inja_json(data.crootref());
    return env.render_file(filename, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
}

// RapidYAML overloads
/// @brief Executes try_render.

std::string try_render(inja::Environment &env, ryml::csubstr input, ryml::ConstNodeRef data)
{
  return try_render(env, std::string_view(input.data(), input.len), data);

}
/// @brief Executes try_render.

std::string try_render(inja::Environment &env, ryml::ConstNodeRef input, ryml::ConstNodeRef data)
{
  return try_render(env, input.val<std::string>().value(), data);

}

/// @brief Executes try_render.

std::string try_render(inja::Environment &env, const std::string &input, ryml::ConstNodeRef data)
{
  return try_render(env, std::string_view(input), data);

}
/// @brief Executes try_render.

std::string try_render(inja::Environment &env, std::string_view input, ryml::ConstNodeRef data)
{
  try {

    return env.render(input, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

/// @brief Executes try_render_file.

std::string try_render_file(inja::Environment &env, const std::filesystem::path &file_path, ryml::ConstNodeRef data)
{
  return try_render_file(env, file_path.string(), data);

}

/// @brief Executes add_common_template_commands.

ryml::NodeRef aggregate_blueprint_data(inja::Environment &env, ryml::ConstNodeRef summary,
                                      c4::csubstr path, ryml::NodeRef output)
{
  const std::string path_text = ryml_string(path);
  const ryml::Pointer pointer{path_text};
  const auto append = [&](ryml::ConstNodeRef source) {
    if (!source.is_map() || !source.contains(pointer))
      return;
    const auto value = source.at(pointer);
    if (value.is_map()) {
      if (output.is_seq())
        throw std::invalid_argument("aggregate cannot mix map and sequence values");
      output |= ryml::MAP;
      merge_node_default(output, value);
    } else {
      if (output.is_map())
        throw std::invalid_argument("aggregate cannot mix map and sequence values");
      output |= ryml::SEQ;
      const auto append_value = [&](ryml::ConstNodeRef item) {
        if (item.is_map() || item.is_seq())
          copy_node_recursive(item, output.append_child());
        else
          output.append_child() << env.render(item.val<std::string>().value(), summary);
      };
      if (value.is_seq())
        for (const auto item: value.children())
          append_value(item);
      else
        append_value(value);
    }
  };
  if (summary.has_child("components"))
    for (const auto component: summary["components"].children())
      append(component);
  if (summary.has_child("data"))
    append(summary["data"]);
  if (!output.is_map() && !output.is_seq())
    output |= ryml::SEQ;
  return output;
}

void add_common_template_commands(inja::Environment &inja_env)
{
  inja_env.add_callback("glob", [](inja::Arguments &args, inja::NodeRef additional_data) {
    ryml::NodeRef aggregate = additional_data["values"].append_child();
    aggregate |= ryml::SEQ;

    std::vector<std::string> string_args;
    for (const auto i: args)
      string_args.push_back(i.val<std::string>().value());
    for (auto &p: glob::rglob(string_args))
      aggregate.append_child() << p.generic_string();
    return aggregate;
  });
/// @brief Executes add_callback.

  inja_env.add_callback("absolute_dir", 1, [](inja::Arguments &args, inja::NodeRef additional_data) {
    const auto path = std::filesystem::path{ args[0].val<std::string>().value() };
    return additional_data["values"].append_child() << std::filesystem::absolute(path).generic_string();

  });
/// @brief Executes add_callback.

  inja_env.add_callback("absolute_path", 1, [](inja::Arguments &args, inja::NodeRef additional_data) {
    const auto path = std::filesystem::path{ args[0].val<std::string>().value() };
    return additional_data["values"].append_child() << std::filesystem::absolute(path).generic_string();

  });
/// @brief Executes add_callback.

  inja_env.add_callback("relative_path", 1, [](inja::Arguments &args, inja::NodeRef additional_data) {
    auto path          = std::filesystem::path{ args[0].val<std::string>().value() };
    const auto current = std::filesystem::current_path();

    auto new_path      = std::filesystem::relative(path, current);
    return additional_data["values"].append_child() << new_path.generic_string();
  });
/// @brief Executes add_callback.

  inja_env.add_callback("relative_path", 2, [](inja::Arguments &args, inja::NodeRef additional_data) {
    const auto path1 = args[0].val<std::string>().value();
    const auto path2 = std::filesystem::absolute(args[1].val<std::string>().value());

    return additional_data["values"].append_child() << std::filesystem::relative(path1, path2).generic_string();
  });
/// @brief Executes add_callback.

  inja_env.add_callback("filesize", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    return additional_data["values"].append_child() << fs::file_size(args[0].val<std::string>().value());
  });

/// @brief Executes add_callback.

  inja_env.add_callback("file_exists", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    return additional_data["values"].append_child() << fs::exists(args[0].val<std::string>().value());
  });

/// @brief Executes add_callback.

  inja_env.add_callback("read_file", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    auto file = std::ifstream(args[0].val<std::string>().value());
    return additional_data["values"].append_child() << std::string{ std::istreambuf_iterator<char>{ file }, {} };

  });
/// @brief Executes add_callback.

  inja_env.add_callback("load_yaml", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    const auto file_path = args[0].val<std::string>().value();
    if (std::filesystem::exists(file_path)) {

      auto file_content = yakka::get_file_contents<std::string>(file_path);
      auto new_node     = additional_data["values"].append_child();
      ryml::parse_in_arena(ryml::to_csubstr(*file_content), new_node);
      return new_node;
    }
    return ryml::NodeRef{};
  });
/// @brief Executes add_callback.

  inja_env.add_callback("load_xml", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    const auto file_path = args[0].val<std::string>().value();
    if (std::filesystem::exists(file_path)) {

      pugi::xml_document doc;
      pugi::xml_parse_result result = doc.load_file(file_path.c_str());
      if (result.status == pugi::xml_parse_status::status_ok) {
        // return xml_to_json(doc.child("device")).crootref();
        // TODO
        throw std::runtime_error("XML loading not implemented yet");
      }
    }
    return ryml::NodeRef{};
  });
/// @brief Executes add_callback.

  inja_env.add_callback("load_json", 1, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    const auto file_path = args[0].val<std::string>().value();
    if (std::filesystem::exists(file_path)) {

      auto file_content = yakka::get_file_contents<std::string>(file_path);
      auto new_node     = additional_data["values"].append_child();
      ryml::parse_in_arena(ryml::to_csubstr(*file_content), new_node);
      return new_node;
    }
    return ryml::NodeRef{};
  });
/// @brief Executes add_callback.

  inja_env.add_callback("merge", 2, [](const inja::Arguments &args, inja::NodeRef additional_data) {
    auto target = args[0];
    // const auto data = args[1];

    // TODO:
    // target.update(data, true);
    return target;
  });
}

/// @brief Executes download_resource.

std::pair<std::string, int> download_resource(const std::string url, std::filesystem::path destination)
{
  std::filesystem::path filename = destination / url.substr(url.find_last_not_of('/'));

#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  return exec("powershell", std::format("Invoke-WebRequest {} -OutFile {}", url, filename.generic_string()));
#else
  return exec("curl", std::format("{} -o {}", url, filename.generic_string()));
#endif
}

#ifdef NOT_USED
/// @brief Executes create_condition_pointer.

ryml::Pointer create_condition_pointer(const ryml::Tree condition)
{
  ryml::Pointer pointer;


  auto root = condition.crootref();
  if (!root.valid()) {
    return pointer;
  }

  for (const auto &item: root.children()) {
    auto value = item.val<std::string>().value();
    if (value.empty()) {
      continue;
    }
    pointer /= ryml::Pointer("/supports/features");
    pointer /= value;
  }

  return pointer;
}
#endif

// Helper struct to hold component comparison results
struct ComparisonResult {
  bool changed{ false };
  std::string error_message;
};

/// @brief Executes has_data_dependency_changed.

[[nodiscard]]
std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, ryml::ConstNodeRef left, ryml::ConstNodeRef right) noexcept
{

  if (data_path.empty() || data_path[0] != data_dependency_identifier) {
    return false;
  }

  if (data_path.size() < 2 || data_path[1] != '/') {
    return std::unexpected{ "Invalid path format: missing root separator" };
  }

  auto left_root  = left;
  auto right_root = right;

  // Early return if left data is missing
  if (!left_root.valid() || !left_root.contains("components")) {
    return true;
  }

  ryml::Pointer data_pointer{ data_path.substr(1) };
  const auto &parts = data_pointer.tokens();

  if (parts.empty()) {
    return false;
  }

  const auto resolve_pointer = [](ryml::ConstNodeRef node, const ryml::Pointer &pointer) {
    for (const auto token: pointer.tokens()) {
      if (!node.valid()) {
        return ryml::ConstNodeRef{};
      }
      if (node.is_map()) {
        if (!node.has_child(token)) {
          return ryml::ConstNodeRef{};
        }
        node = node.find_child(token);
      } else if (node.is_seq()) {
        size_t index = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || index >= node.num_children()) {
          return ryml::ConstNodeRef{};
        }
        node = node[index];
      } else {
        return ryml::ConstNodeRef{};
      }
    }
    return node;
  };

  const ryml::csubstr first_part  = parts.size() > 0 ? parts[0] : ryml::csubstr{};
  const ryml::csubstr second_part = parts.size() > 1 ? parts[1] : ryml::csubstr{};
  ryml::Pointer remaining_pointer;
  if (parts.size() > 2) {
    remaining_pointer = ryml::Pointer{ std::vector<ryml::csubstr>(parts.begin() + 2, parts.end()) };
  }

  const auto left_components  = left_root["components"];
  const auto right_components = right_root["components"];

  // Define a lambda to process a component name using the captured 'left' and 'right' json objects
  const auto process_component = [&](ryml::csubstr component_name, const ryml::Pointer &pointer) -> ComparisonResult {
    if (!left_components.valid() || !right_components.valid() || !left_components.is_map() || !right_components.is_map()) {
      return { true, "" };
    }

    c4::csubstr key = component_name;
    if (!left_components.has_child(key) || !right_components.has_child(key)) {
      return { true, "" };
    }

    auto left_comp  = left_components.find_child(key);
    auto right_comp = right_components.find_child(key);

    auto left_value  = resolve_pointer(left_comp, pointer);
    auto right_value = resolve_pointer(right_comp, pointer);

    if (!left_value.valid() && !right_value.valid()) {
      return { false, "" };
    }
    if (!left_value.valid() || !right_value.valid()) {
      return { true, "" };
    }

    auto left_serialized  = ryml::emitrs_json<std::string>(left_value);
    auto right_serialized = ryml::emitrs_json<std::string>(right_value);
    return { left_serialized != right_serialized, "" };
  };

  try {
    if (first_part == "components") {
      if (second_part == yakka::data_wildcard_identifier) {
        auto right_components = right_root["components"];
        if (!right_components.valid() || !right_components.is_map()) {
          return true;
        }

        for (const auto child: right_components.children()) {
          if (!child.has_key()) {
            continue;
          }
          auto result = process_component(child.key(), remaining_pointer);
          if (!result.error_message.empty()) {
            return std::unexpected{ std::move(result.error_message) };
          }
          if (result.changed) {
            return true;
          }
        }

        const auto left_components = left_root["components"];
        for (const auto child: left_components.children()) {
          if (!child.has_key() || right_components.has_child(child.key())) {
            continue;
          }
          auto result = process_component(child.key(), remaining_pointer);
          if (!result.error_message.empty()) {
            return std::unexpected{ std::move(result.error_message) };
          }
          if (result.changed) {
            return true;
          }
        }
      } else {
        ryml::csubstr component_name = c4::to_csubstr(second_part);
        auto result                  = process_component(component_name, remaining_pointer);
        if (!result.error_message.empty()) {
          return std::unexpected{ std::move(result.error_message) };
        }
        return result.changed;
      }
    } else {
      auto left_value  = resolve_pointer(left_root, data_pointer);
      auto right_value = resolve_pointer(right_root, data_pointer);
      if (!left_value.valid() || !right_value.valid()) {
        return true;
      }
      auto left_serialized  = ryml::emitrs_json<std::string>(left_value);
      auto right_serialized = ryml::emitrs_json<std::string>(right_value);
      return { left_serialized != right_serialized };
    }

    return false;
  } catch (const std::exception &e) {
    return std::unexpected{ std::string{ "Failed to determine data dependency: " } + e.what() };
  }
}

/// @brief Executes hash_file.

void hash_file(std::filesystem::path filename, uint8_t out_hash[32]) noexcept
{
  try {

    auto start = std::chrono::steady_clock::now();

    std::ifstream file(filename, std::ios::binary);
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer))) {
      blake3_hasher_update(&hasher, buffer, file.gcount());
    }
    if (file.gcount() > 0) {
      blake3_hasher_update(&hasher, buffer, file.gcount());
    }
    blake3_hasher_finalize(&hasher, out_hash, BLAKE3_OUT_LEN);

    auto end = std::chrono::steady_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    spdlog::info("Hashed {} in {} ms", filename.generic_string(), ms);
  } catch (std::exception &e) {
    spdlog::error("Failed to hash file {}: {}", filename.generic_string(), e.what());
    std::fill(out_hash, out_hash + 32, 0);
  }
}

/* TODO. Clean this up and optimize. 
 * This is a very naive implementation that creates a new tree for each node and merges it into the parent. It also uses a lot of string copying. 
 * A more efficient implementation would build the tree in place without merging and avoid unnecessary string copies.
 */

/// @brief Executes xml_to_json.

void xml_to_json(const pugi::xml_node &node, ryml::NodeRef &target)
{
  target |= ryml::MAP;

  // Add attributes
  for (auto &attr: node.attributes()) {
    std::string key = "_" + std::string(attr.name());
    // target[c4::to_csubstr(key)] << attr.value();
    target.append_child() << ryml::key(key) << attr.value();
  }

  // Add children
  for (auto &child: node.children()) {
    if (child.name()[0] == 0) {
      continue;
    }

    ryml::NodeRef seq_node = target[child.name()];
    if (!seq_node.valid()) {
      seq_node = target.append_child();
      seq_node |= ryml::SEQ;
      seq_node.set_key_serialized(child.name());
    }

    auto new_child = seq_node.append_child();
    new_child |= ryml::MAP;
    xml_to_json(child, new_child);
  }

  // Add text content
  const char *text_value = node.text().get();
  if (text_value != nullptr && *text_value != '\0') {
    auto child = target.append_child();
    child << ryml::key("_text");
    child << text_value;
  }
}

// Load file content and parse using RapidYAML
/// @brief Executes ryml_load_file.

std::expected<ryml::Tree, std::error_code> ryml_load_file(const std::filesystem::path &path)
{
  auto file_content = yakka::get_file_contents<std::string>(path);

  if (!file_content) {
    return std::unexpected(file_content.error());
  }

  try {
    return ryml::parse_in_arena(ryml::to_csubstr(*file_content));
  } catch (...) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
}

// RapidYAML to ryml::Tree conversion (clone subtree)
// ryml::Tree ryml_to_json(ryml::ConstNodeRef node)
// {
//   ryml::Tree result;
//   if (!node.valid()) {
//     return result;
//   }

//   result.merge_with(node.tree(), node.id(), result.root_id());
//   return result;
// }

// RapidYAML to inja::json conversion (for templating)
// inja::json ryml_to_inja_json(ryml::ConstNodeRef node)
// {
//   if (!node.valid()) {
//     return inja::json{};
//   }

//   if (node.is_map()) {
//     inja::json obj = inja::json::object();
//     for (const auto &child : node.children()) {
//       if (!child.has_key()) {
//         continue;
//       }
//       std::string key;
//       c4::from_chars(child.key(), &key);
//       obj[key] = ryml_to_inja_json(child);
//     }
//     return obj;
//   }

//   if (node.is_seq()) {
//     inja::json arr = inja::json::array();
//     for (const auto &child : node.children()) {
//       arr.push_back(ryml_to_inja_json(child));
//     }
//     return arr;
//   }

//   if (node.has_val()) {
//     std::string val_str;
//     c4::from_chars(node.val(), &val_str);

//     if (val_str == "null" || val_str == "~" || val_str.empty()) {
//       return nullptr;
//     }

//     if (val_str == "true" || val_str == "yes" || val_str == "on") {
//       return true;
//     }
//     if (val_str == "false" || val_str == "no" || val_str == "off") {
//       return false;
//     }

//     if (!val_str.empty() && (std::isdigit(val_str[0]) || val_str[0] == '-' || val_str[0] == '+')) {
//       try {
//         if (val_str.find('.') == std::string::npos && val_str.find('e') == std::string::npos && val_str.find('E') == std::string::npos) {
//           return std::stoll(val_str);
//         }
//         return std::stod(val_str);
//       } catch (...) {
//         // fall back to string
//       }
//     }

//     return val_str;
//   }

//   return inja::json{};
// }

// Helper function to check if ryml node has a child with given key
/// @brief Executes ryml_has_child.

bool ryml_has_child(ryml::ConstNodeRef node, c4::csubstr key)
{
  if (!node.valid() || !node.is_map()) {

    return false;
  }
  return node.has_child(key);
}

// ryml::ConstNodeRef ryml_get_path(ryml::ConstNodeRef node, const ryml::Pointer &path)
// {
//   if (!node.valid()) {
//     return ryml::ConstNodeRef();
//   }

//   ryml::ConstNodeRef current = node;
//   for (const auto &segment: path.tokens()) {
//     if (segment.empty()) {
//       continue;
//     }

//     if (current.is_seq()) {
//       size_t index = 0;
//       try {
//         index = static_cast<size_t>(std::stoul(segment.data()));
//       } catch (...) {
//         spdlog::error("Non-numeric index '{}' used for sequence navigation", segment);
//         return ryml::ConstNodeRef();
//       }

//       if (index >= current.num_children()) {
//         return ryml::ConstNodeRef();
//       }

//       current = current.child(index);
//       continue;
//     }

//     if (!current.is_map()) {
//       return ryml::ConstNodeRef();
//     }

//     c4::csubstr key = c4::to_csubstr(segment);
//     if (!current.has_child(key)) {
//       return ryml::ConstNodeRef();
//     }
//     current = current.find_child(key);
//   }

//   return current;
// }

// bool ryml_has_path(ryml::ConstNodeRef node, const ryml::Pointer &path)
// {
//   return ryml_get_path(node, path).valid();
// }

/// @brief Executes ryml_path.

std::filesystem::path ryml_path(c4::csubstr path)
{
  std::string path_str;

  c4::from_chars(path, &path_str);
  return fs::path{ path_str };
}

// Merge two ryml nodes
/// @brief Executes ryml_node_merge.

void ryml_node_merge(ryml::ConstNodeRef source, ryml::NodeRef target, const schema *schema)
{
  // For now, implement basic merging

  // Full implementation would need to handle merge strategies from schema
  if (!source.valid() || !target.valid()) {
    return;
  }

  if (source.is_map() && target.is_map()) {
    for (const auto &child: source.children()) {
      if (!child.has_key())
        continue;

      c4::csubstr key = child.key();
      if (target.has_child(key)) {
        ryml::NodeRef target_child = target.find_child(key);
        if (child.is_map() && target_child.is_map()) {
          ryml_node_merge(child, target_child, schema);
        } else {
          // Overwrite with new value - copy the node value
          if (child.has_val()) {
            target_child.set_val(child.val());
          }
        }
      } else {
        // Add new key - this is complex, simplified for now
        // In a full implementation, would need to properly copy the entire subtree
        if (child.has_val()) {
          ryml::NodeRef new_child = target.append_child();
          new_child << ryml::key(key);
          new_child << child.val();
        }
      }
    }
  } else if (source.is_seq() && target.is_seq()) {
    // For sequences, append all elements
    for (const auto &child: source.children()) {
      if (child.has_val()) {
        ryml::NodeRef new_child = target.append_child();
        new_child << child.val();
      }
    }
  }
}

/**
 * Navigate to a specific path in a ryml tree, creating nodes if needed
 * @param node The root node to navigate from
 * @param path Vector of path segments (e.g., {"data", "settings"})
 * @param create_if_missing If true, create missing nodes along the path
 * @return NodeRef to the node at the specified path, or invalid NodeRef if not found and not created
 */
// ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const std::vector<std::string> &path, bool create_if_missing)
// {
//   if (!node.valid()) {
//     return node;
//   }

//   ryml::NodeRef current = node;
//   for (const auto &segment : path) {
//     if (segment.empty()) continue; // Skip empty segments (e.g., from leading "/")

//     if (current.is_seq()) {
//       size_t index = 0;
//       try {
//         index = static_cast<size_t>(std::stoul(segment));
//       } catch (...) {
//         spdlog::error("Non-numeric index '{}' used for sequence navigation", segment);
//         return ryml::NodeRef();
//       }

//       if (index < current.num_children()) {
//         current = current.child(index);
//         continue;
//       }

//       if (!create_if_missing) {
//         spdlog::warn("Sequence index '{}' not found", segment);
//         return ryml::NodeRef();
//       }

//       while (current.num_children() <= index) {
//         ryml::NodeRef new_child = current.append_child();
//         new_child |= ryml::MAP; // Default to map type for newly created children
//       }
//       current = current.child(index);
//       continue;
//     }

//     c4::csubstr key = c4::to_csubstr(segment);

//     if (current.has_child(key)) {
//       current = current.find_child(key);
//     } else if (create_if_missing) {
//       // Ensure current node is a map
//       if (!current.is_map()) {
//         if (current.is_seed()) {
//           current |= ryml::MAP;
//         } else {
//           spdlog::error("Cannot navigate through non-map node at path segment '{}'", segment);
//           return ryml::NodeRef(); // Return invalid node
//         }
//       }

//       // Create new child
//       ryml::NodeRef new_child = current.append_child();
//       new_child << ryml::key(key);
//       new_child |= ryml::MAP; // Default to map type
//       current = new_child;
//     } else {
//       spdlog::warn("Path segment '{}' not found", segment);
//       return ryml::NodeRef(); // Return invalid node
//     }
//   }

//   return current;
// }

// ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const ryml::Pointer &path, bool create_if_missing)
// {
//   return ryml_navigate_path(node, path.parts(), create_if_missing);
// }

/// @brief Executes ryml_save_file.

void ryml_save_file(const std::filesystem::path &path, ryml::ConstNodeRef node)
{
  try {
    write_file_if_changed(path, ryml::emitrs_yaml<std::string>(node));
  } catch (const std::exception &e) {
    spdlog::error("Error saving ryml tree to file {}: {}", path.generic_string(), e.what());
  }
}

void write_file_if_changed(const std::filesystem::path &path, std::string_view content)
{
  const auto existing = get_file_contents<std::string>(path);
  if (existing && *existing == content)
    return;

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    spdlog::error("Failed to open file for writing: {}", path.generic_string());
    return;
  }
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!file)
    spdlog::error("Failed to write file: {}", path.generic_string());
}

} // namespace yakka
