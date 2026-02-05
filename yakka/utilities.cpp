#include "yakka_project.hpp"
#include "utilities.hpp"
#include "subprocess.hpp"
#include "spdlog/spdlog.h"
#include "glob/glob.h"
#include "yakka_schema.hpp"
#include "blake3.h"
#include "pugixml.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <concepts>
#include <string_view>
#include <expected>
#include <fstream>
#include <string>
#include <vector>
#include <ranges>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <filesystem>

namespace yakka {

/*
// Execute with admin
if(0 == CreateProcess(argv[2], params, NULL, NULL, false, 0, NULL, NULL, &si, &pi)) {
        //runas word is a hack to require UAC elevation
        ShellExecute(NULL, "runas", argv[2], params, NULL, SW_SHOWNORMAL);
}
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

YAML::Node yaml_path(const YAML::Node &node, std::string path)
{
  YAML::Node temp = node;
  std::stringstream ss(path);
  std::string s;
  while (std::getline(ss, s, '.'))
    temp.reset(temp[s]);
  return temp;
}

// RymlPointer ryml_pointer(std::string path)
// {
//   if (path.front() != '/') {
//     path = "/" + path;
//     std::replace(path.begin(), path.end(), '.', '/');
//   }
//   return RymlPointer{ path };
// }

ryml::Tree json_path(const ryml::Tree &node, std::string path)
{
  RymlPointer temp(path);
  return node[temp];

  // return node[RymlPointer(path)];
  // auto temp = node;
  // std::stringstream ss(path);
  // std::string s;
  // while (std::getline(ss, s, '.'))
  //     temp = temp[s];//.reset(temp[s]);
  // return temp;
}

static std::string unescape_ryml_pointer_segment(std::string_view segment)
{
  std::string result;
  result.reserve(segment.size());
  for (size_t i = 0; i < segment.size(); ++i) {
    if (segment[i] == '~' && i + 1 < segment.size()) {
      char next = segment[i + 1];
      if (next == '0') {
        result.push_back('~');
        ++i;
        continue;
      }
      if (next == '1') {
        result.push_back('/');
        ++i;
        continue;
      }
    }
    result.push_back(segment[i]);
  }
  return result;
}

static std::vector<std::string> parse_ryml_pointer_segments(std::string path)
{
  std::vector<std::string> segments;
  if (path.empty()) {
    return segments;
  }

  if (path.front() != '/') {
    std::replace(path.begin(), path.end(), '.', '/');
    path = "/" + path;
  }

  std::string_view path_view(path);
  size_t start = 1;
  while (start <= path_view.size()) {
    size_t slash = path_view.find('/', start);
    if (slash == std::string_view::npos) {
      slash = path_view.size();
    }
    auto segment_view = path_view.substr(start, slash - start);
    segments.emplace_back(unescape_ryml_pointer_segment(segment_view));
    if (slash >= path_view.size()) {
      break;
    }
    start = slash + 1;
  }

  return segments;
}

RymlPointer::RymlPointer(std::string path) : segments(parse_ryml_pointer_segments(std::move(path)))
{
}

RymlPointer::RymlPointer(std::vector<std::string> segments_in) : segments(std::move(segments_in))
{
}

const std::vector<std::string> &RymlPointer::parts() const noexcept
{
  return segments;
}

bool RymlPointer::empty() const noexcept
{
  return segments.empty();
}

RymlPointer ryml_pointer(std::string path)
{
  return RymlPointer{ std::move(path) };
}

std::tuple<component_list_t, feature_list_t, command_list_t> parse_arguments(const std::vector<std::string> &argument_string)
{
  component_list_t components;
  feature_list_t features;
  command_list_t commands;

  //    for (auto s = argument_string.begin(); s != argument_string.end(); ++s)
  for (auto s: argument_string) {
    // Identify features, commands, and components
    if (s.front() == '+')
      features.insert(s.substr(1));
    else if (s.back() == '!')
      commands.insert(s.substr(0, s.size() - 1));
    else
      components.insert(s);
  }

  return { std::move(components), std::move(features), std::move(commands) };
}

std::string generate_project_name(const component_list_t &components, const feature_list_t &features)
{
  std::string project_name = "";

  // Generate the project name from the project string
  for (const auto &i: components)
    project_name += i + "-";

  if (!components.empty())
    project_name.pop_back();

  for (const auto &i: features)
    project_name += "-" + i;

  if (project_name.empty())
    project_name = "none";

  return project_name;
}

/**
 * @brief Parses dependency files as output by GCC or Clang generating a vector of filenames as found in the named file
 *
 * @param filename  Name of the dependency file. Typically ending in '.d'
 * @return std::vector<std::string>  Vector of files specified as dependencies
 */
std::vector<std::string> parse_gcc_dependency_file(const std::string &filename)
{
  std::vector<std::string> dependencies;
  std::ifstream infile(filename);

  if (!infile.is_open())
    return {};

  std::string line;

  // Find and ignore the first line with the target. Typically "<target>: \"
  do {
    std::getline(infile, line);
  } while (line.length() > 0 && line.find(':') == std::string::npos);

  while (std::getline(infile, line, ' ')) {
    if (line.empty() || line.compare("\\\n") == 0)
      continue;
    if (line.back() == '\n')
      line.pop_back();
    if (line.back() == '\r')
      line.pop_back();
    dependencies.push_back(line.starts_with("./") ? line.substr(line.find_first_not_of("/", 2)) : line);
  }

  return dependencies;
}

/**
 * @param path  Path relative to Yakka component schema
 */
void json_node_merge(RymlPointer path, ryml::Tree &merge_target, const ryml::Tree &node, const schema* schema)
{
  // Check for a strategy for this path
  schema::merge_strategy strategy = (schema != nullptr) ? schema->get_merge_strategy(path) : schema::merge_strategy::Default;

  switch (node.type()) {
    case nlohmann::detail::value_t::object:
      if (merge_target.type() != nlohmann::detail::value_t::object) {
        spdlog::error("Currently not supported");
        return;
      }
      // Iterate through child nodes
      for (auto it = node.begin(); it != node.end(); ++it) {
        // Check if the key is already in merge_target
        auto it2 = merge_target.find(it.key());
        if (it2 != merge_target.end()) {
          json_node_merge(path / it.key(), it2.value(), it.value(), schema);
        } else {
          merge_target[it.key()] = it.value();
        }
      }
      break;

    case nlohmann::detail::value_t::array:
      switch (merge_target.type()) {
        case nlohmann::detail::value_t::object:
          spdlog::error("Cannot merge array into an object");
          break;
        default:
          // Convert scalar into an array
          merge_target = ryml::Tree::array({ merge_target });
          [[fallthrough]];
        case nlohmann::detail::value_t::array:
        case nlohmann::detail::value_t::null:
          for (auto &i: node)
            merge_target.push_back(i);
          break;
      }
      break;

    case nlohmann::detail::value_t::null:
      break;

    default:
      switch (merge_target.type()) {
        case nlohmann::detail::value_t::object:
          spdlog::error("Cannot merge scalar into an object");
          break;
        case nlohmann::detail::value_t::array:
          merge_target.push_back(node);
          break;
        default:
          if (strategy == schema::merge_strategy::Abort) {
            spdlog::error("Conflict detected while merging scalar values at path {}", path.to_string());
            return;
          } else if (strategy == schema::merge_strategy::Overwrite) {
            merge_target = node;
          } else if (strategy == schema::merge_strategy::Max && node.is_number() && merge_target.is_number()) {
            merge_target = std::max(merge_target, node);
          } else if (strategy == schema::merge_strategy::Min && node.is_number() && merge_target.is_number()) {
            merge_target = std::min(merge_target, node);
          } else {
            merge_target = ryml::Tree::array({ merge_target });
            merge_target.push_back(node);
          }
          break;
      }
      break;
  }

  // Apply addition merge strategy
  // Check if there is an additional stategy, if not return
  // if (auto schema = schema_validator::get_schema(path)) {
  //   spdlog::error("Found schema");
  // } else {
  //   spdlog::error("No schema found for path {}", path.to_string());
  // }
}

std::string component_dotname_to_id(const std::string dotname)
{
  return dotname.find_last_of(".") != std::string::npos ? dotname.substr(dotname.find_last_of(".") + 1) : dotname;
}

std::string try_render(inja::Environment &env, const std::string &input, const ryml::Tree &data)
{
  try {
    return env.render(input, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

std::string try_render_file(inja::Environment &env, const std::string &filename, const ryml::Tree &data)
{
  try {
    return env.render_file(filename, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
}

// RapidYAML overloads
std::string try_render(inja::Environment &env, const std::string &input, const ryml::ConstNodeRef &data)
{
  try {
    // TODO: Convert ryml to format inja can consume
    // For now, convert to ryml::Tree as bridge
    ryml::Tree json_data = ryml_to_json(data);
    return env.render(input, json_data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

std::string try_render_file(inja::Environment &env, const std::string &filename, const ryml::ConstNodeRef &data)
{
  try {
    // TODO: Convert ryml to format inja can consume
    ryml::Tree json_data = ryml_to_json(data);
    return env.render_file(filename, json_data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
}
#endif
}

void add_common_template_commands(inja::Environment &inja_env)
{
  inja_env.add_callback("dir", 1, [](inja::Arguments &args) {
    auto path = std::filesystem::path{ args.at(0)->get<std::string>() };
    return path.has_filename() ? path.parent_path().string() : path.string();
  });
  inja_env.add_callback("not_dir", 1, [](inja::Arguments &args) {
    return std::filesystem::path{ args.at(0)->get<std::string>() }.filename().string();
  });
  inja_env.add_callback("parent_path", 1, [](inja::Arguments &args) {
    return std::filesystem::path{ args.at(0)->get<std::string>() }.parent_path().string();
  });
  inja_env.add_callback("glob", [](inja::Arguments &args) {
    ryml::Tree aggregate = ryml::Tree::array();
    std::vector<std::string> string_args;
    for (const auto &i: args)
      string_args.push_back(i->get<std::string>());
    for (auto &p: glob::rglob(string_args))
      aggregate.push_back(p.generic_string());
    return aggregate;
  });
  inja_env.add_callback("absolute_dir", 1, [](inja::Arguments &args) {
    const auto path = std::filesystem::path{ args.at(0)->get<std::string>() };
    return std::filesystem::absolute(path).generic_string();
  });
  inja_env.add_callback("absolute_path", 1, [](inja::Arguments &args) {
    const auto path = std::filesystem::path{ args.at(0)->get<std::string>() };
    return std::filesystem::absolute(path).generic_string();
  });
  inja_env.add_callback("relative_path", 1, [](inja::Arguments &args) {
    auto path          = std::filesystem::path{ args.at(0)->get<std::string>() };
    const auto current = std::filesystem::current_path();
    auto new_path      = std::filesystem::relative(path, current);
    return new_path.generic_string();
  });
  inja_env.add_callback("relative_path", 2, [](inja::Arguments &args) {
    const auto path1 = args.at(0)->get<std::string>();
    const auto path2 = std::filesystem::absolute(args.at(1)->get<std::string>());
    return std::filesystem::relative(path1, path2).generic_string();
  });
  inja_env.add_callback("extension", 1, [](inja::Arguments &args) {
    return std::filesystem::path{ args.at(0)->get<std::string>() }.extension().string().substr(1);
  });
  inja_env.add_callback("filesize", 1, [](const inja::Arguments &args) {
    return fs::file_size(args[0]->get<std::string>());
  });
  inja_env.add_callback("file_exists", 1, [](const inja::Arguments &args) {
    return fs::exists(args[0]->get<std::string>());
  });
  inja_env.add_callback("hex2dec", 1, [](const inja::Arguments &args) {
    std::string hex_string = args[0]->get<std::string>();
    return std::stoul(hex_string, nullptr, 16);
  });
  inja_env.add_callback("read_file", 1, [](const inja::Arguments &args) {
    auto file = std::ifstream(args[0]->get<std::string>());
    return std::string{ std::istreambuf_iterator<char>{ file }, {} };
  });
  inja_env.add_callback("load_yaml", 1, [](const inja::Arguments &args) {
    const auto file_path = args[0]->get<std::string>();
    if (std::filesystem::exists(file_path)) {
      auto yaml_data = YAML::LoadFile(file_path);
      return yaml_data.as<ryml::Tree>();
    } else {
      return ryml::Tree();
    }
  });
  inja_env.add_callback("load_xml", 1, [](const inja::Arguments &args) {
    const auto file_path = args[0]->get<std::string>();
    if (std::filesystem::exists(file_path)) {
      pugi::xml_document doc;
      pugi::xml_parse_result result = doc.load_file(file_path.c_str());
      if (result.status == pugi::xml_parse_status::status_ok) {
        return xml_to_json(doc.child("device"));
      }
    }
    return ryml::Tree();
  });
  inja_env.add_callback("load_json", 1, [](const inja::Arguments &args) {
    const auto file_path = args[0]->get<std::string>();
    if (std::filesystem::exists(file_path)) {
      std::ifstream file_stream(file_path);
      return ryml::Tree::parse(file_stream,
                                   /* callback */ nullptr,
                                   /* allow exceptions */ false,
                                   /* ignore_comments */ true,
                                   /* ignore_trailing_commas */ true);
    } else {
      return ryml::Tree();
    }
  });
  inja_env.add_callback("quote", 1, [](const inja::Arguments &args) {
    std::stringstream ss;
    if (args[0]->is_string())
      ss << std::quoted(args[0]->get<std::string>());
    else if (args[0]->is_number_integer())
      ss << std::quoted(std::to_string(args[0]->get<int>()));
    else if (args[0]->is_number_float())
      ss << std::quoted(std::to_string(args[0]->get<float>()));
    return ss.str();
  });
  inja_env.add_callback("replace", 3, [](const inja::Arguments &args) {
    auto input  = args[0]->get<std::string>();
    auto target = std::regex(args[1]->get<std::string>());
    auto match  = args[2]->get<std::string>();
    return std::regex_replace(input, target, match);
  });
  inja_env.add_callback("regex_escape", 1, [](const inja::Arguments &args) {
    auto input = args[0]->get<std::string>();
    const std::regex metacharacters(R"([\.\^\$\+\(\)\[\]\{\}\|\?])");
    return std::regex_replace(input, metacharacters, "\\$&");
  });
  inja_env.add_callback("split", 2, [](const inja::Arguments &args) {
    auto input = args[0]->get<std::string>();
    auto delim = args[1]->get<std::string>();
    ryml::Tree output;
    for (auto word: std::views::split(input, delim))
      output.push_back(std::string_view(word));
    return output;
  });
  inja_env.add_callback("starts_with", 2, [](const inja::Arguments &args) {
    auto input = args[0]->get<std::string>();
    auto start = args[1]->get<std::string>();
    return input.starts_with(start);
  });
  inja_env.add_callback("substring", 2, [](const inja::Arguments &args) {
    auto input = args[0]->get<std::string>();
    auto index = args[1]->get<int>();
    return input.substr(index);
  });
  inja_env.add_callback("trim", 1, [](const inja::Arguments &args) {
    auto input = args[0]->get<std::string>();
    input.erase(input.begin(), std::find_if(input.begin(), input.end(), [](unsigned char ch) {
                  return !std::isspace(ch);
                }));
    input.erase(std::find_if(input.rbegin(),
                             input.rend(),
                             [](unsigned char ch) {
                               return !std::isspace(ch);
                             })
                  .base(),
                input.end());
    return input;
  });
  inja_env.add_callback("filter", 2, [](const inja::Arguments &args) {
    json output            = ryml::Tree::array();
    const auto input       = args[0];
    const auto regex_match = std::regex(args[1]->get<std::string>());
    std::copy_if(input->begin(), input->end(), std::back_inserter(output), [&](const auto &item) {
      return std::regex_match(item.template get<std::string>(), regex_match);
    });
    return output;
  });
  inja_env.add_callback("join", 2, [](const inja::Arguments &args) {
    const auto input = args[0];
    if (input->empty() || !input->is_array()) {
      spdlog::error("join() expects an array as the first argument");
      return std::string{};
    }
    const auto separator = args[1]->get<std::string>();
    // Add the first element (input already checked to be not empty)
    auto it            = input->begin();
    std::string output = it->template get<std::string>();
    // Iterate through the rest of the elements and append them with the separator
    for (++it; it != input->end(); ++it) {
      output += separator + it->template get<std::string>();
    }
    return output;
  });
  inja_env.add_callback("find_json", 2, [](const inja::Arguments &args) {
    const auto input      = args[0];
    const auto search_key = args[1]->get<std::string>();
    json output;
    find_json_keys(input->get<ryml::Tree>(), search_key, "", output);
    return output;
  });
  inja_env.add_callback("merge", 2, [](const inja::Arguments &args) {
    auto target     = args[0]->get<ryml::Tree>();
    const auto data = args[1]->get<ryml::Tree>();
    target.update(data, true);
    return target;
  });
  inja_env.add_callback("concatenate", [](const inja::Arguments &args) {
    std::string aggregate;
    for (const auto &i: args)
      aggregate.append(i->get<std::string>());
    return aggregate;
  });
}

std::pair<std::string, int> download_resource(const std::string url, std::filesystem::path destination)
{
  std::filesystem::path filename = destination / url.substr(url.find_last_not_of('/'));
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  return exec("powershell", std::format("Invoke-WebRequest {} -OutFile {}", url, filename.generic_string()));
#else
  return exec("curl", std::format("{} -o {}", url, filename.generic_string()));
#endif
}

RymlPointer create_condition_pointer(const ryml::Tree condition)
{
  RymlPointer pointer;

  for (const auto &item: condition) {
    pointer /= ryml_pointer("/supports/features");
    pointer /= item.get<std::string>();
  }

  return pointer;
}

// Helper struct to hold component comparison results
struct ComparisonResult {
  bool changed{ false };
  std::string error_message;
};

[[nodiscard]]
std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, const ryml::Tree &left, const ryml::Tree &right) noexcept
{
  if (data_path.empty() || data_path[0] != data_dependency_identifier) {
    return false;
  }

  if (data_path[1] != '/') {
    return std::unexpected{ "Invalid path format: missing root separator" };
  }

  // Early return if left data is missing
  if (left.is_null() || left["components"].is_null()) {
    return true;
  }

  auto data_paths = std::ranges::views::split(data_path, '/') | std::views::drop(1);
  RymlPointer data_pointer{ data_path.substr(1) };

  auto iter = data_paths.begin();
  const std::string first_part{ std::string_view(*iter) };
  const std::string second_part{ std::string_view(*(++iter)) };
  RymlPointer remaining_pointer;
  for (const auto &part: data_paths | std::views::drop(2)) {
    remaining_pointer /= std::string(std::string_view{ part });
  }

  // Define a lambda to process a component name using the captured 'left' and 'right' json objects
  const auto process_component = [&](std::string_view component_name, const RymlPointer &pointer) -> ComparisonResult {
    if (!left["components"].contains(component_name) || !right["components"].contains(component_name)) {
      return { true, "" };
    }

    const auto &left_comp  = left["components"][std::string{ component_name }];
    const auto &right_comp = right["components"][std::string{ component_name }];

    auto get_value = [](const auto &json, const auto &ptr) {
      return json.contains(ptr) ? json[ptr] : ryml::Tree{};
    };

    auto left_value  = get_value(left_comp, pointer);
    auto right_value = get_value(right_comp, pointer);

    return { left_value != right_value, "" };
  };

  try {
    if (first_part == "components") {
      if (second_part == std::string{ data_wildcard_identifier }) {

        // Using C++20 ranges to process components
        for (const auto &[component_name, _]: right["components"].items()) {
          auto result = process_component(component_name, remaining_pointer);
          if (!result.error_message.empty()) {
            return std::unexpected{ std::move(result.error_message) };
          }
          return result.changed;
        }
      } else {
        auto component_name = second_part;
        auto result         = process_component(component_name, remaining_pointer);
        if (!result.error_message.empty()) {
          return std::unexpected{ std::move(result.error_message) };
        }
        return result.changed;
      }
    } else {
      if (!left.contains(data_pointer) || !right.contains(data_pointer)) {
        return true;
      }
      // auto diff = json::diff(left[data_pointer], right[data_pointer]);
      // if (!diff.empty()) {
      //   spdlog::error("Data dependency changed at path: {}", data_pointer.to_string());
      // }
      return { left[data_pointer] != right[data_pointer] };
    }

    return false;
  } catch (const std::exception &e) {
    return std::unexpected{ std::string{ "Failed to determine data dependency: " } + e.what() };
  }
}

void find_json_keys(const ryml::Tree &j, const std::string &target_key, const std::string &current_path, ryml::Tree &paths)
{
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      std::string new_path = current_path.empty() ? it.key() : current_path + "." + it.key();

      if (it.key() == target_key) {
        paths.push_back(new_path); // Store full path to the key
      }

      find_json_keys(it.value(), target_key, new_path, paths);
    }
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); ++i) {
      std::string new_path = current_path + "[" + std::to_string(i) + "]";
      find_json_keys(j[i], target_key, new_path, paths);
    }
  }
}

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

ryml::Tree xml_to_json(const pugi::xml_node &node)
{
  ryml::Tree j;
  // Add attributes
  for (auto &attr: node.attributes()) {
    j["_" + std::string(attr.name())] = attr.value();
  }
  // Add children
  for (auto &child: node.children()) {
    if (child.name()[0] != 0) {
      j[child.name()].push_back(xml_to_json(child));
    }
  }
  // Add text content
  if (node.text()) {
    j["_text"] = node.text().get();
  }
  return j;
}

// Load file content and parse using RapidYAML
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

// RapidYAML to ryml::Tree conversion (only used when interfacing with inja or schema validator)
ryml::Tree ryml_to_json(const ryml::ConstNodeRef &node)
{
  if (!node.valid()) {
    return ryml::Tree();
  }

  if (node.is_map()) {
    ryml::Tree j = ryml::Tree::object();
    for (const auto &child : node.children()) {
      std::string key;
      if (child.has_key()) {
        c4::from_chars(child.key(), &key);
        j[key] = ryml_to_json(child);
      }
    }
    return j;
  } else if (node.is_seq()) {
    ryml::Tree j = ryml::Tree::array();
    for (const auto &child : node.children()) {
      j.push_back(ryml_to_json(child));
    }
    return j;
  } else if (node.has_val()) {
    // Try to parse as different types
    std::string val_str;
    c4::from_chars(node.val(), &val_str);
    
    // Check for null
    if (val_str == "null" || val_str == "~" || val_str.empty()) {
      return ryml::Tree();
    }
    
    // Check for boolean
    if (val_str == "true" || val_str == "yes" || val_str == "on") {
      return ryml::Tree(true);
    }
    if (val_str == "false" || val_str == "no" || val_str == "off") {
      return ryml::Tree(false);
    }
    
    // Try to parse as number
    if (!val_str.empty() && (std::isdigit(val_str[0]) || val_str[0] == '-' || val_str[0] == '+')) {
      try {
        // Try integer first
        if (val_str.find('.') == std::string::npos && 
            val_str.find('e') == std::string::npos && 
            val_str.find('E') == std::string::npos) {
          return ryml::Tree(std::stoll(val_str));
        } else {
          return ryml::Tree(std::stod(val_str));
        }
      } catch (...) {
        // Not a number, treat as string
      }
    }
    
    // Default to string
    return ryml::Tree(val_str);
  }

  return ryml::Tree();
}

// Helper function to get value as string from ryml node
std::string ryml_get_val_as_string(const ryml::ConstNodeRef &node)
{
  if (!node.valid() || !node.has_val()) {
    return "";
  }
  std::string result;
  c4::from_chars(node.val(), &result);
  return result;
}

// Helper function to check if ryml node has a child with given key
bool ryml_has_child(const ryml::ConstNodeRef &node, c4::csubstr key)
{
  if (!node.valid() || !node.is_map()) {
    return false;
  }
  return node.has_child(key);
}

// Helper function to get child node by key
ryml::ConstNodeRef ryml_get_child(const ryml::ConstNodeRef &node, c4::csubstr key)
{
  if (!node.valid() || !node.is_map()) {
    return ryml::ConstNodeRef();
  }
  return node.find_child(key);
}

ryml::ConstNodeRef ryml_get_path(const ryml::ConstNodeRef &node, const RymlPointer &path)
{
  if (!node.valid()) {
    return ryml::ConstNodeRef();
  }

  ryml::ConstNodeRef current = node;
  for (const auto &segment : path.parts()) {
    if (segment.empty()) {
      continue;
    }

    if (current.is_seq()) {
      size_t index = 0;
      try {
        index = static_cast<size_t>(std::stoul(segment));
      } catch (...) {
        spdlog::error("Non-numeric index '{}' used for sequence navigation", segment);
        return ryml::ConstNodeRef();
      }

      if (index >= current.num_children()) {
        return ryml::ConstNodeRef();
      }

      current = current.child(index);
      continue;
    }

    if (!current.is_map()) {
      return ryml::ConstNodeRef();
    }

    c4::csubstr key = c4::to_csubstr(segment);
    if (!current.has_child(key)) {
      return ryml::ConstNodeRef();
    }
    current = current.find_child(key);
  }

  return current;
}

bool ryml_has_path(const ryml::ConstNodeRef &node, const RymlPointer &path)
{
  return ryml_get_path(node, path).valid();
}

// Merge two ryml nodes
void ryml_node_merge(const ryml::ConstNodeRef &source, ryml::NodeRef target, const schema* schema)
{
  // For now, implement basic merging
  // Full implementation would need to handle merge strategies from schema
  if (!source.valid() || !target.valid()) {
    return;
  }

  if (source.is_map() && target.is_map()) {
    for (const auto &child : source.children()) {
      if (!child.has_key()) continue;
      
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
          new_child.set_key(key);
          new_child.set_val(child.val());
        }
      }
    }
  } else if (source.is_seq() && target.is_seq()) {
    // For sequences, append all elements
    for (const auto &child : source.children()) {
      if (child.has_val()) {
        ryml::NodeRef new_child = target.append_child();
        new_child.set_val(child.val());
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
ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const std::vector<std::string> &path, bool create_if_missing)
{
  if (!node.valid()) {
    return node;
  }

  ryml::NodeRef current = node;
  for (const auto &segment : path) {
    if (segment.empty()) continue; // Skip empty segments (e.g., from leading "/")

    if (current.is_seq()) {
      size_t index = 0;
      try {
        index = static_cast<size_t>(std::stoul(segment));
      } catch (...) {
        spdlog::error("Non-numeric index '{}' used for sequence navigation", segment);
        return ryml::NodeRef();
      }

      if (index < current.num_children()) {
        current = current.child(index);
        continue;
      }

      if (!create_if_missing) {
        spdlog::warn("Sequence index '{}' not found", segment);
        return ryml::NodeRef();
      }

      while (current.num_children() <= index) {
        ryml::NodeRef new_child = current.append_child();
        new_child |= ryml::MAP; // Default to map type for newly created children
      }
      current = current.child(index);
      continue;
    }

    c4::csubstr key = c4::to_csubstr(segment);

    if (current.has_child(key)) {
      current = current.find_child(key);
    } else if (create_if_missing) {
      // Ensure current node is a map
      if (!current.is_map()) {
        if (current.is_seed()) {
          current |= ryml::MAP;
        } else {
          spdlog::error("Cannot navigate through non-map node at path segment '{}'", segment);
          return ryml::NodeRef(); // Return invalid node
        }
      }

      // Create new child
      ryml::NodeRef new_child = current.append_child();
      new_child << ryml::key(key);
      new_child |= ryml::MAP; // Default to map type
      current = new_child;
    } else {
      spdlog::warn("Path segment '{}' not found", segment);
      return ryml::NodeRef(); // Return invalid node
    }
  }
  
  return current;
}

ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const RymlPointer &path, bool create_if_missing)
{
  return ryml_navigate_path(node, path.parts(), create_if_missing);
}

/**
 * Merge ryml nodes at a specific path (overload of json_node_merge for ryml)
 * @param path Vector of path segments relative to merge_target (e.g., {"data", "settings"})
 * @param merge_target The target node to merge into
 * @param node The source node to merge from
 * @param schema Optional schema for merge strategy
 */
void json_node_merge(const std::vector<std::string> &path, ryml::NodeRef merge_target, const ryml::ConstNodeRef &node, const schema* schema)
{
  if (!merge_target.valid() || !node.valid()) {
    spdlog::error("Invalid nodes in json_node_merge (ryml version)");
    return;
  }

  // Navigate to the target location, creating nodes if necessary
  ryml::NodeRef target = ryml_navigate_path(merge_target, path, true);
  
  if (!target.valid()) {
    spdlog::error("Failed to navigate to path in json_node_merge (ryml version)");
    return;
  }

  // Get merge strategy from schema if available
  // Note: This requires converting path to RymlPointer format for schema lookup
  schema::merge_strategy strategy = schema::merge_strategy::Default;
  if (schema != nullptr) {
    // Build RymlPointer from path vector
    std::string pointer_str;
    for (const auto &segment : path) {
      if (!segment.empty()) {
        pointer_str += "/" + segment;
      }
    }
    if (pointer_str.empty()) {
      pointer_str = "";
    }
    try {
      RymlPointer json_ptr(pointer_str);
      strategy = schema->get_merge_strategy(json_ptr);
    } catch (...) {
      // If pointer construction fails, use default strategy
      strategy = schema::merge_strategy::Default;
    }
  }

  // Perform the merge based on node types
  if (node.is_map()) {
    if (!target.is_map()) {
      spdlog::error("Cannot merge map into non-map node");
      return;
    }
    
    // Merge map nodes
    for (const auto &child : node.children()) {
      if (!child.has_key()) continue;
      
      c4::csubstr key = child.key();
      std::string key_str(key.data(), key.size());
      
      if (target.has_child(key)) {
        ryml::NodeRef target_child = target.find_child(key);
        
        // Build new path for recursive merge
        std::vector<std::string> child_path = path;
        child_path.push_back(key_str);
        
        // Recursively merge
        json_node_merge(child_path, merge_target, child, schema);
      } else {
        // Add new key-value pair
        // TODO: Implement full subtree copy for complex nodes
        if (child.has_val()) {
          ryml::NodeRef new_child = target.append_child();
          new_child << ryml::key(key);
          new_child.set_val(child.val());
        }
      }
    }
  } else if (node.is_seq()) {
    // Handle sequence merging
    if (target.is_map()) {
      spdlog::error("Cannot merge sequence into map node");
      return;
    }
    
    // Convert target to sequence if it's not already
    if (!target.is_seq()) {
      if (target.is_seed() || !target.has_val()) {
        target |= ryml::SEQ;
      } else {
        // Target has a scalar value, convert to array with that value
        c4::csubstr existing_val = target.val();
        target |= ryml::SEQ;
        ryml::NodeRef first = target.append_child();
        first.set_val(existing_val);
      }
    }
    
    // Append all sequence elements
    for (const auto &child : node.children()) {
      if (child.has_val()) {
        ryml::NodeRef new_child = target.append_child();
        new_child.set_val(child.val());
      }
    }
  } else if (node.has_val()) {
    // Handle scalar values
    if (target.is_map()) {
      spdlog::error("Cannot merge scalar into map node");
      return;
    } else if (target.is_seq()) {
      // Append to sequence
      ryml::NodeRef new_child = target.append_child();
      new_child.set_val(node.val());
    } else {
      // Merge scalar into scalar based on strategy
      if (strategy == schema::merge_strategy::Abort) {
        std::string path_str;
        for (const auto &seg : path) {
          path_str += "/" + seg;
        }
        spdlog::error("Conflict detected while merging scalar values at path {}", path_str);
        return;
      } else if (strategy == schema::merge_strategy::Overwrite) {
        target.set_val(node.val());
      } else if (strategy == schema::merge_strategy::Max || strategy == schema::merge_strategy::Min) {
        // For numeric comparisons, convert to numbers
        // TODO: Implement numeric comparison logic
        target.set_val(node.val()); // Default to overwrite for now
      } else {
        // Default: convert to array
        c4::csubstr existing_val = target.has_val() ? target.val() : c4::csubstr("");
        target |= ryml::SEQ;
        if (!existing_val.empty()) {
          ryml::NodeRef first = target.append_child();
          first.set_val(existing_val);
        }
        ryml::NodeRef second = target.append_child();
        second.set_val(node.val());
      }
    }
  }
}

} // namespace yakka
