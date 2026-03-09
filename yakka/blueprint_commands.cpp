#include "blueprint_commands.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "yakka.hpp"
#include <regex>
#include <charconv>

namespace yakka {

process_return echo_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (command.valid() && command.has_val())
    captured_output = try_render(inja_env, command.val<std::string>().value(), project_summary);

  spdlog::get("console")->info("{}", captured_output);
  return { captured_output, 0 };
}

// blueprint_commands["execute"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return execute_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val())
    return { "", -1 };
  std::string temp = command.val<std::string>().value();
  try {
    captured_output = try_render(inja_env, temp, project_summary);
    //std::replace( captured_output.begin( ), captured_output.end( ), '/', '\\' );
    spdlog::debug("Executing '{}'", captured_output);
    auto [temp_output, retcode] = exec(captured_output, std::string(""));

    if (retcode != 0 && temp_output.length() != 0) {
      spdlog::error("\n{} returned {}\n{}", captured_output, retcode, temp_output);
    } else if (temp_output.length() != 0)
      spdlog::info("{}", temp_output);
    return { temp_output, retcode };
  } catch (std::exception &e) {
    spdlog::error("Failed to execute: {}\n{}", temp, e.what());
    captured_output = "";
    return { "", -1 };
  }
}

// blueprint_commands["shell"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return shell_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val())
    return { "", -1 };
  std::string temp = command.val<std::string>().value();
  try {
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
    captured_output = "cmd /k \"" + try_render(inja_env, temp, project_summary) + "\"";
#else
    captured_output = try_render(inja_env, temp, project_summary);
#endif
    spdlog::debug("Executing '{}' in a shell", captured_output);
    auto [temp_output, retcode] = exec(captured_output, std::string(""));

    if (retcode != 0 && temp_output.length() != 0) {
      spdlog::error("\n{} returned {}\n{}", captured_output, retcode, temp_output);
    } else if (temp_output.length() != 0)
      spdlog::info("{}", temp_output);
    return { temp_output, retcode };
  } catch (std::exception &e) {
    spdlog::error("Failed to execute: {}\n{}", temp, e.what());
    captured_output = "";
    return { "", -1 };
  }
};

// blueprint_commands["regex"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return regex_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  const auto search_node = command.find_child("search");
  if (!search_node.valid() || !search_node.has_val()) {
    spdlog::error("'regex' command missing 'search'");
    return { "", -1 };
  }
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  std::regex regex_search(search_node.val<std::string>().value());
#else
  std::regex regex_search(search_node.val<std::string>().value(), std::regex::multiline);
#endif
  const auto prefix_node = command.find_child("prefix");
  const auto suffix_node = command.find_child("suffix");
  std::string prefix     = prefix_node.valid() ? try_render(inja_env, prefix_node.val<std::string>().value(), project_summary) : "";
  std::string suffix     = suffix_node.valid() ? try_render(inja_env, suffix_node.val<std::string>().value(), project_summary) : "";

  if (command.has_child("split")) {
    std::istringstream ss(captured_output);
    std::string line;
    captured_output = "";

    while (std::getline(ss, line)) {
      if (command.has_child("replace")) {
        const auto replace_node = command.find_child("replace");
        std::string r           = std::regex_replace(line, regex_search, replace_node.val<std::string>().value(), std::regex_constants::format_no_copy);
        captured_output.append(r);
      } else if (command.has_child("match")) {
        std::smatch sm;
        std::string new_output      = prefix_node.valid() ? prefix_node.val<std::string>().value() : "";
        inja::Environment local_env = inja_env; // Create copy and override `$()` function
        const auto match_string     = command.find_child("match");
        local_env.add_callback("reg", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
          auto node = additional_data.rootref().append_child() << sm[args[0].val<int>().value()].str();
          return node;
        });
        for (; std::regex_search(captured_output, sm, regex_search);) {
          // Render the match template
          new_output += try_render(local_env, match_string, project_summary);
          captured_output = sm.suffix();
        }
        if (suffix_node.valid())
          new_output += suffix_node.val<std::string>().value();
        captured_output = new_output;
      } else if (command.has_child("to_yaml")) {
        std::smatch s;
        if (!std::regex_match(line, s, regex_search))
          continue;
        YAML::Node node;
        node[0]                 = YAML::Node();
        int i                   = 1;
        const auto to_yaml_node = command.find_child("to_yaml");
        for (const auto &v: to_yaml_node.children()) {
          node[0][v.val<std::string>().value()] = s[i++].str();
        }

        if (prefix_node.valid())
          captured_output.append(try_render(inja_env, prefix_node.val<std::string>().value(), project_summary));
        captured_output.append(YAML::Dump(node));
        captured_output.append("\n");
        if (suffix_node.valid())
          captured_output.append(try_render(inja_env, suffix_node.val<std::string>().value(), project_summary));
      }
    }
  } else if (command.has_child("to_yaml")) {
    YAML::Node yaml;
    for (std::smatch sm; std::regex_search(captured_output, sm, regex_search);) {
      YAML::Node new_node;
      int i                   = 1;
      const auto to_yaml_node = command.find_child("to_yaml");
      for (const auto &v: to_yaml_node.children()) {
        new_node[v.val<std::string>().value()] = sm[i++].str();
      }
      yaml.push_back(new_node);
      captured_output = sm.suffix();
    }

    captured_output.erase();
    captured_output = prefix + YAML::Dump(yaml) + "\n" + suffix;
    // captured_output.append(prefix);
    // captured_output.append(YAML::Dump(yaml));
    // captured_output.append("\n");
    // captured_output.append(suffix);
  } else if (command.has_child("replace")) {
    const auto replace_node = command.find_child("replace");
    captured_output         = prefix + std::regex_replace(captured_output, regex_search, replace_node.val<std::string>().value()) + suffix;
  } else if (command.has_child("match")) {
    std::smatch sm;
    std::string new_output      = prefix;
    inja::Environment local_env = inja_env; // Create copy and override `$()` function
    const auto match_string     = command.find_child("match");
    local_env.add_callback("reg", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
      auto node = additional_data.rootref().append_child() << sm[args[0].val<int>().value()].str();
      return node;
    });
    for (; std::regex_search(captured_output, sm, regex_search);) {
      // Render the match template
      new_output += try_render(local_env, match_string, project_summary);
      captured_output = sm.suffix();
    }
    captured_output = new_output + suffix;
    //captured_output = std::regex_replace(captured_output, regex_search, command["match"].get<std::string>(), std::regex_constants::format_no_copy);
  } else {
    spdlog::error("'regex' command does not have enough information");
    return { "", -1 };
  }
  return { captured_output, 0 };
};

// blueprint_commands["template"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return template_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  try {
    std::string template_string;
    std::string template_filename;
    ryml::Tree data;
    auto data_root   = data.crootref();
    auto render_data = [&]() -> ryml::ConstNodeRef {
      data_root = data.crootref();
      if (data_root.valid() && (data_root.has_val() || data_root.num_children() > 0)) {
        return data_root;
      }
      return project_summary;
    };

    if (command.valid() && command.has_val() && !command.is_map() && !command.is_seq()) {
      captured_output = try_render(inja_env, command.val<std::string>().value(), project_summary);
      return { captured_output, 0 };
    }

    if (command.valid() && command.is_map()) {
      if (command.has_child("data_file")) {
        std::filesystem::path data_filename = try_render(inja_env, command["data_file"], project_summary);
        auto loaded                         = ryml_load_file(data_filename);
        if (loaded) {
          data = std::move(*loaded);
        }
      } else if (command.has_child("data")) {
        std::string data_string = try_render(inja_env, command["data"], project_summary);
        try {
          data = ryml::parse_in_arena(ryml::to_csubstr(data_string));
        } catch (...) {
          spdlog::error("Failed to parse inline data for template");
        }
      }

      if (command.has_child("template_file")) {
        template_filename = try_render(inja_env, command["template_file"], project_summary);
        captured_output   = try_render_file(inja_env, template_filename, render_data());
        return { captured_output, 0 };
      }

      if (command.has_child("template")) {
        template_string = try_render(inja_env, command["template"], project_summary);
        captured_output = try_render(inja_env, template_string, render_data());
        return { captured_output, 0 };
      }
    }

    spdlog::error("Inja template is invalid:\n'{}'", ryml::emitrs_yaml<std::string>(command));
    return { "", -1 };
  } catch (std::exception &e) {
    spdlog::error("Failed to apply template: {}\n{}", ryml::emitrs_yaml<std::string>(command), e.what());
    return { "", -1 };
  }
  return { captured_output, 0 };
};

// Backwards compatibility with old 'inja' command
// blueprint_commands["inja"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return inja_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  return template_command(target, command, captured_output, project_summary, project_data, inja_env);
};

// blueprint_commands["save"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return save_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  std::string save_filename;

  if (!command.valid() || (!command.has_val() && command.num_children() == 0))
    save_filename = target;
  else
    save_filename = try_render(inja_env, command.val<std::string>().value(), project_summary);

  // Special case to save data depenencies
  if (!save_filename.empty() && save_filename[0] == data_dependency_identifier) {
    if (save_filename.rfind(":/data/", 0) != 0) {
      spdlog::error("Data dependency pointer must start with '/data'");
      return { "", -1 };
    }
    auto pointer     = ryml::Pointer{ save_filename.substr(6) };
    auto target_node = project_data[pointer]; //ryml_navigate_path(project_data, pointer, true);
    if (target_node.valid()) {
      target_node.set_val(c4::to_csubstr(captured_output));
    }
    return { captured_output, 0 };
  }

  try {
    std::ofstream save_file;
    std::filesystem::path p(save_filename);
    if (!p.parent_path().empty())
      fs::create_directories(p.parent_path());
    save_file.open(save_filename, std::ios_base::binary);
    if (!save_file.is_open()) {
      spdlog::error("Failed to save file: '{}'", save_filename);
      return { "", -1 };
    }
    save_file << captured_output;
    save_file.flush();
    save_file.close();
  } catch (std::exception &e) {
    spdlog::error("Failed to save file: '{}'", save_filename);
    return { "", -1 };
  }
  return { captured_output, 0 };
};

// blueprint_commands["create_directory"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return create_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (command.valid() && command.has_val()) {
    std::string filename = "";
    try {
      filename = command.val<std::string>().value();
      filename = try_render(inja_env, filename, project_summary);
      if (!filename.empty()) {
        std::filesystem::path p(filename);
        fs::create_directories(p.parent_path());
      }
    } catch (std::exception &e) {
      spdlog::error("Couldn't create directory for '{}'", filename);
      return { "", -1 };
    }
  }
  return { "", 0 };
}

// blueprint_commands["verify"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return verify_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val()) {
    return { "", -1 };
  }
  std::string filename = command.val<std::string>().value();
  filename             = try_render(inja_env, filename, project_summary);
  if (fs::exists(filename)) {
    spdlog::info("{} exists", filename);
    return { captured_output, 0 };
  }

  spdlog::info("BAD!! {} doesn't exist", filename);
  return { "", -1 };
};

// blueprint_commands["rm"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return rm_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val()) {
    return { "", -1 };
  }
  std::string filename = command.val<std::string>().value();
  filename             = try_render(inja_env, filename, project_summary);
  // Check if the input was a YAML array construct
  if (filename.front() == '[' && filename.back() == ']') {
    // Load the generated dependency string as YAML and push each item individually
    try {
      auto file_list = YAML::Load(filename);
      for (auto i: file_list) {
        const auto file = i.Scalar();
        fs::remove(file);
      }
    } catch (std::exception &e) {
      spdlog::error("Failed to parse file list: {}", filename);
    }
  } else {
    fs::remove(filename);
  }
  return { captured_output, 0 };
};

// blueprint_commands["rmdir"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return rmdir_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val()) {
    return { "", -1 };
  }
  std::string path = command.val<std::string>().value();
  path             = try_render(inja_env, path, project_summary);
  // Put some checks here
  std::error_code ec;
  fs::remove_all(path, ec);
  if (!ec) {
    spdlog::error("'rmdir' command failed {}\n", ec.message());
  }
  return { captured_output, 0 };
};

// blueprint_commands["pack"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return pack_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  std::vector<std::byte> data_output;

  const auto data_node   = command.find_child("data");
  const auto format_node = command.find_child("format");
  if (!data_node.valid()) {
    spdlog::error("'pack' command requires 'data'\n");
    return { "", -1 };
  }
  if (!format_node.valid() || !format_node.has_val()) {
    spdlog::error("'pack' command requires 'format'\n");
    return { "", -1 };
  }

  std::string format = format_node.val<std::string>().value();
  format             = try_render(inja_env, format, project_summary);

  auto i = format.begin();
  for (const auto &d: data_node.children()) {
    auto v       = try_render(inja_env, d.val<std::string>().value(), project_summary);
    const char c = *i++;
    union {
      int8_t s8;
      uint8_t u8;
      int16_t s16;
      uint16_t u16;
      int32_t s32;
      uint32_t u32;
      unsigned long value;
      std::byte bytes[8];
    } temp;
    const auto result = (v.size() > 1 && v[1] == 'x') ? std::from_chars(v.data() + 2, v.data() + v.size(), temp.u32, 16)
                        : (v[0] == '-')               ? std::from_chars(v.data(), v.data() + v.size(), temp.s32)
                                                      : std::from_chars(v.data(), v.data() + v.size(), temp.u32);
    if (result.ec != std::errc()) {
      spdlog::error("Error converting number: {}\n", v);
    }

    switch (c) {
      case 'L':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[4]);
        break;
      case 'l':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[4]);
        break;
      case 'S':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[2]);
        break;
      case 's':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[2]);
        break;
      case 'C':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[1]);
        break;
      case 'c':
        data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[1]);
        break;
      case 'x':
        data_output.push_back(std::byte{ 0 });
        break;
      default:
        spdlog::error("Unknown pack type\n");
        break;
    }
  }
  auto *chars = reinterpret_cast<char const *>(data_output.data());
  captured_output.insert(captured_output.end(), chars, chars + data_output.size());
  return { captured_output, 0 };
};

// blueprint_commands["copy"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return copy_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  std::string destination;
  ryml::Tree source_tree;
  ryml::ConstNodeRef source;
  try {
    auto destination_node = command.find_child("destination");
    if (!destination_node.valid() || !destination_node.has_val()) {
      spdlog::error("'copy' command missing 'destination'");
      return { "", -1 };
    }
    destination = try_render(inja_env, destination_node.val<std::string>().value(), project_summary);

    if (command.has_child("source")) {
      source = command.find_child("source");
    } else if (command.has_child("yaml_list")) {
      auto yaml_list_node = command.find_child("yaml_list");
      if (!yaml_list_node.has_val()) {
        spdlog::error("'copy' command 'yaml_list' is not a string");
        return { "", -1 };
      }
      std::string list_yaml_string = try_render(inja_env, yaml_list_node.val<std::string>().value(), project_summary);
      try {
        source_tree = ryml::parse_in_arena(ryml::to_csubstr(list_yaml_string));
        source      = source_tree.crootref();
      } catch (...) {
        spdlog::error("'copy' command failed to parse 'yaml_list'");
        return { "", -1 };
      }
    } else {
      spdlog::error("'copy' command does not have 'source' or 'yaml_list'");
      return { "", -1 };
    }

    if (!source.valid()) {
      spdlog::error("'copy' command has invalid 'source'");
      return { "", -1 };
    }

    if (source.has_val() && !source.is_map() && !source.is_seq()) {
      auto source_string = try_render(inja_env, source.val<std::string>().value(), project_summary);
      std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
    } else if (source.is_seq()) {
      for (const auto &f: source.children()) {
        auto source_string = try_render(inja_env, f.val<std::string>().value(), project_summary);
        std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
      }
    } else if (source.is_map()) {
      if (source.has_child("folder_paths"))
        for (const auto &f: source.find_child("folder_paths").children()) {
          auto source_string = try_render(inja_env, f.val<std::string>().value(), project_summary);
          auto dest          = destination + "/" + source_string;
          std::filesystem::create_directories(dest);
          std::filesystem::copy(source_string, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
        }
      if (source.has_child("folders"))
        for (const auto &f: source.find_child("folders").children()) {
          auto source_string = try_render(inja_env, f.val<std::string>().value(), project_summary);
          std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
        }
      if (source.has_child("file_paths"))
        for (const auto &f: source.find_child("file_paths").children()) {
          auto source_string         = try_render(inja_env, f.val<std::string>().value(), project_summary);
          std::filesystem::path dest = destination + "/" + source_string;
          std::filesystem::create_directories(dest.parent_path());
          std::filesystem::copy(source_string, dest, std::filesystem::copy_options::update_existing);
        }
      if (source.has_child("files"))
        for (const auto &f: source.find_child("files").children()) {
          auto source_string = try_render(inja_env, f.val<std::string>().value(), project_summary);
          std::filesystem::copy(source_string, destination, std::filesystem::copy_options::update_existing);
        }
    } else {
      spdlog::error("'copy' command missing 'source' or 'list' while processing {}", target);
      return { "", -1 };
    }
  } catch (std::exception &e) {
    spdlog::error("'copy' command failed while processing {}: '{}' -> '{}'\r\n{}", target, ryml::emitrs_yaml<std::string>(source), destination, e.what());
    return { "", -1 };
  }
  return { "", 0 };
}

// blueprint_commands["cat"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return cat_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.has_val()) {
    return { "", -1 };
  }
  std::string filename = try_render(inja_env, command.val<std::string>().value(), project_summary);
  std::ifstream datafile;
  datafile.open(filename, std::ios_base::in | std::ios_base::binary);
  std::stringstream string_stream;
  string_stream << datafile.rdbuf();
  captured_output = string_stream.str();
  datafile.close();
  return { captured_output, 0 };
}

// blueprint_commands["new_project"] = [this](std::string target, const ryml::Tree &command, std::string captured_output, const ryml::Tree &project_summary, ryml::Tree &project_data, inja::Environment &inja_env) -> yakka::process_return {
//   const auto project_string = command.get<std::string>();
//   yakka::project new_project(project_string, workspace);
//   new_project.init_project(project_string);
//   return { "", 0 };
// };

// blueprint_commands["as_json"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return as_json_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  try {
    ryml::Tree temp_tree = ryml::parse_in_arena(ryml::to_csubstr(captured_output));
    return { ryml::emitrs_json<std::string>(temp_tree), 0 };
  } catch (std::exception &e) {
    spdlog::error("Failed to parse JSON: {}", e.what());
    return { "", -1 };
  }
}

// blueprint_commands["as_yaml"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return as_yaml_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  try {
    // parse_in_place requires a mutable buffer
    auto cs         = ryml::to_substr(captured_output);
    ryml::Tree t    = ryml::parse_in_place(cs);
    std::string out = ryml::emitrs_yaml<std::string>(t.rootref());
    return { out, 0 };
  } catch (std::exception &e) {
    spdlog::error("Failed to parse/emit YAML: {}", e.what());
    return { "", -1 };
  }
}
// blueprint_commands["diff"] = [](std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return diff_command(std::string target, ryml::ConstNodeRef command, std::string captured_output, ryml::ConstNodeRef project_summary, ryml::NodeRef &project_data, inja::Environment &inja_env)
{
  if (!command.valid() || !command.is_map()) {
    spdlog::error("'diff' command invalid");
    return { "", -1 };
  }
  ryml::Tree left;
  ryml::Tree right;
  bool has_left  = false;
  bool has_right = false;

  if (command.has_child("left_file")) {
    const std::filesystem::path left_file = try_render(inja_env, command.find_child("left_file"), project_summary);
    auto loaded                           = ryml_load_file(left_file);
    if (loaded) {
      left     = std::move(*loaded);
      has_left = true;
    }
  } else if (command.has_child("left")) {
    std::string left_string = try_render(inja_env, command.find_child("left"), project_summary);
    try {
      left     = ryml::parse_in_arena(ryml::to_csubstr(left_string));
      has_left = true;
    } catch (...) {
      spdlog::error("Failed to parse 'left' content");
    }
  }

  if (command.has_child("right_file")) {
    const std::filesystem::path right_file = try_render(inja_env, command.find_child("right_file"), project_summary);
    auto loaded                            = ryml_load_file(right_file);
    if (loaded) {
      right     = std::move(*loaded);
      has_right = true;
    }
  } else if (command.has_child("right")) {
    std::string right_string = try_render(inja_env, command.find_child("right"), project_summary);
    try {
      right     = ryml::parse_in_arena(ryml::to_csubstr(right_string));
      has_right = true;
    } catch (...) {
      spdlog::error("Failed to parse 'right' content");
    }
  }

  if (!has_left || !has_right) {
    spdlog::error("'diff' command requires both left and right values");
    return { "", -1 };
  }

  // auto left_json  = ryml_to_inja_json(left.crootref());
  // auto right_json = ryml_to_inja_json(right.crootref());
  // auto patch = inja::json::diff(left_json, right_json);
  // return { patch.dump(), 0 };
  return { "", 0 }; // TODO: implement diffing
}

const std::map<const std::string, const blueprint_command> blueprint_commands = {
  // clang-format off
  { "echo", echo_command },
  { "execute", execute_command },
  { "shell", shell_command },
  { "regex", regex_command },
  { "inja", inja_command },
  { "template", template_command },
  { "save", save_command },
  { "create_directory", create_command },
  { "verify", verify_command },
  { "rm", rm_command },
  { "rmdir", rmdir_command },
  { "pack", pack_command },
  { "copy", copy_command },
  { "cat", cat_command },
  { "as_json", as_json_command },
  { "as_yaml", as_yaml_command },
  { "diff", diff_command }
  // clang-format on
};

} // namespace yakka