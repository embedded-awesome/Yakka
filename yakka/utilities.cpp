#include "yakka_project.hpp"
#include "utilities.hpp"
#include "subprocess.hpp"
#include "spdlog/spdlog.h"
#include "glob/glob.h"
#include "yakka_schema.hpp"
#include "blake3.h"
#include "pugixml.hpp"
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

// nlohmann::json::json_pointer json_pointer(std::string path)
// {
//   if (path.front() != '/') {
//     path = "/" + path;
//     std::replace(path.begin(), path.end(), '.', '/');
//   }
//   return nlohmann::json::json_pointer{ path };
// }

nlohmann::json json_path(const nlohmann::json &node, std::string path)
{
  nlohmann::json::json_pointer temp(path);
  return node[temp];

  // return node[nlohmann::json_pointer(path)];
  // auto temp = node;
  // std::stringstream ss(path);
  // std::string s;
  // while (std::getline(ss, s, '.'))
  //     temp = temp[s];//.reset(temp[s]);
  // return temp;
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
void json_node_merge(nlohmann::json::json_pointer path, nlohmann::json &merge_target, const nlohmann::json &node)
{
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
          json_node_merge(""_json_pointer, it2.value(), it.value());
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
          merge_target = nlohmann::json::array({ merge_target });
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
        default:
          // Convert scalar into an array
          merge_target = nlohmann::json::array({ merge_target });
          [[fallthrough]];
        case nlohmann::detail::value_t::array:
          merge_target.push_back(node);
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

std::string try_render(inja::Environment &env, const std::string &input, const nlohmann::json &data)
{
  try {
    return env.render(input, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

std::string try_render_file(inja::Environment &env, const std::string &filename, const nlohmann::json &data)
{
  try {
    return env.render_file(filename, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
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
    nlohmann::json aggregate = nlohmann::json::array();
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
      return yaml_data.as<nlohmann::json>();
    } else {
      return nlohmann::json();
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
    return nlohmann::json();
  });
  inja_env.add_callback("load_json", 1, [](const inja::Arguments &args) {
    const auto file_path = args[0]->get<std::string>();
    if (std::filesystem::exists(file_path)) {
      std::ifstream file_stream(file_path);
      return nlohmann::json::parse(file_stream,
                                   /* callback */ nullptr,
                                   /* allow exceptions */ false,
                                   /* ignore_comments */ true,
                                   /* ignore_trailing_commas */ true);
    } else {
      return nlohmann::json();
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
    nlohmann::json output;
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
    json output            = nlohmann::json::array();
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
    find_json_keys(input->get<nlohmann::json>(), search_key, "", output);
    return output;
  });
  inja_env.add_callback("merge", 2, [](const inja::Arguments &args) {
    auto target     = args[0]->get<nlohmann::json>();
    const auto data = args[1]->get<nlohmann::json>();
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

std::pair<std::string, int> download_resource(const std::string url, fs::path destination)
{
  fs::path filename = destination / url.substr(url.find_last_not_of('/'));
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  return exec("powershell", std::format("Invoke-WebRequest {} -OutFile {}", url, filename.generic_string()));
#else
  return exec("curl", std::format("{} -o {}", url, filename.generic_string()));
#endif
}

nlohmann::json::json_pointer create_condition_pointer(const nlohmann::json condition)
{
  nlohmann::json::json_pointer pointer;

  for (const auto &item: condition) {
    pointer /= "/supports/features"_json_pointer;
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
std::expected<bool, std::string> has_data_dependency_changed(std::string data_path, const nlohmann::json &left, const nlohmann::json &right) noexcept
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
  nlohmann::json::json_pointer data_pointer{ data_path.substr(1) };

  auto iter = data_paths.begin();
  const std::string first_part{ std::string_view(*iter) };
  const std::string second_part{ std::string_view(*(++iter)) };
  nlohmann::json::json_pointer remaining_pointer;
  for (const auto &part: data_paths | std::views::drop(2)) {
    remaining_pointer /= std::string(std::string_view{ part });
  }

  // Define a lambda to process a component name using the captured 'left' and 'right' json objects
  const auto process_component = [&](std::string_view component_name, const nlohmann::json::json_pointer &pointer) -> ComparisonResult {
    if (!left["components"].contains(component_name) || !right["components"].contains(component_name)) {
      return { true, "" };
    }

    const auto &left_comp  = left["components"][std::string{ component_name }];
    const auto &right_comp = right["components"][std::string{ component_name }];

    auto get_value = [](const auto &json, const auto &ptr) {
      return json.contains(ptr) ? json[ptr] : nlohmann::json{};
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
          auto result = process_component(component_name, data_pointer);
          if (!result.error_message.empty()) {
            return std::unexpected{ std::move(result.error_message) };
          }

          if (result.changed == true) {
            spdlog::error("Data dependency changed for component: {}", component_name);
          }
          return result.changed;
        }
      } else {
        auto component_name = second_part;
        auto result         = process_component(component_name, remaining_pointer);
        if (!result.error_message.empty()) {
          return std::unexpected{ std::move(result.error_message) };
        }
        if (result.changed == true) {
          spdlog::error("Data dependency changed for component: {}", component_name);
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

void find_json_keys(const nlohmann::json &j, const std::string &target_key, const std::string &current_path, nlohmann::json &paths)
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

nlohmann::json xml_to_json(const pugi::xml_node &node)
{
  nlohmann::json j;
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

} // namespace yakka
