#include "blueprint_commands.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "yakka.hpp"
#include <regex>
#include <charconv>

namespace yakka {

process_return echo_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  if (!command.is_null())
    captured_output = try_render(inja_env, command.get<std::string>(), project_summary);

  spdlog::get("console")->info("{}", captured_output);
  return { captured_output, 0 };
}

// blueprint_commands["execute"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return execute_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  if (command.is_null())
    return { "", -1 };
  std::string temp = command.get<std::string>();
  try {
    captured_output = inja_env.render(temp, project_summary);
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
};

// blueprint_commands["shell"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return shell_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  if (command.is_null())
    return { "", -1 };
  std::string temp = command.get<std::string>();
  try {
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
    captured_output = "cmd /k \"" + inja_env.render(temp, project_summary) + "\"";
#else
    captured_output = inja_env.render(temp, project_summary);
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

// blueprint_commands["fix_slashes"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
//   std::replace(captured_output.begin(), captured_output.end(), '\\', '/');
//   return { captured_output, 0 };
// };

// blueprint_commands["regex"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return regex_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  assert(command.contains("search"));
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  std::regex regex_search(command["search"].get<std::string>());
#else
  std::regex regex_search(command["search"].get<std::string>(), std::regex::multiline);
#endif
  std::string prefix = command.contains("prefix") ? try_render(inja_env, command["prefix"].get<std::string>(), project_summary) : "";
  std::string suffix = command.contains("suffix") ? try_render(inja_env, command["suffix"].get<std::string>(), project_summary) : "";
  if (command.contains("split")) {
    std::istringstream ss(captured_output);
    std::string line;
    captured_output = "";

    while (std::getline(ss, line)) {
      if (command.contains("replace")) {
        std::string r = std::regex_replace(line, regex_search, command["replace"].get<std::string>(), std::regex_constants::format_no_copy);
        captured_output.append(r);
      } else if (command.contains("match")) {
        std::smatch sm;
        std::string new_output      = command.contains("prefix") ? command["prefix"].get<std::string>() : "";
        inja::Environment local_env = inja_env; // Create copy and override `$()` function
        const auto match_string     = command["match"].get<std::string>();
        local_env.add_callback("reg", 1, [&](const inja::Arguments &args) {
          return sm[args[0]->get<int>()].str();
        });
        for (; std::regex_search(captured_output, sm, regex_search);) {
          // Render the match template
          new_output += try_render(local_env, match_string, project_summary);
          captured_output = sm.suffix();
        }
        if (command.contains("suffix"))
          new_output += command["suffix"].get<std::string>();
        captured_output = new_output;
      } else if (command.contains("to_yaml")) {
        std::smatch s;
        if (!std::regex_match(line, s, regex_search))
          continue;
        YAML::Node node;
        node[0] = YAML::Node();
        int i   = 1;
        for (auto &v: command["to_yaml"])
          node[0][v.get<std::string>()] = s[i++].str();

        if (command.contains("prefix"))
          captured_output.append(try_render(inja_env, command["prefix"].get<std::string>(), project_summary));
        captured_output.append(YAML::Dump(node));
        captured_output.append("\n");
        if (command.contains("suffix"))
          captured_output.append(try_render(inja_env, command["suffix"].get<std::string>(), project_summary));
      }
    }
  } else if (command.contains("to_yaml")) {
    YAML::Node yaml;
    for (std::smatch sm; std::regex_search(captured_output, sm, regex_search);) {
      YAML::Node new_node;
      int i = 1;
      for (auto &v: command["to_yaml"])
        new_node[v.get<std::string>()] = sm[i++].str();
      yaml.push_back(new_node);
      captured_output = sm.suffix();
    }

    captured_output.erase();
    captured_output = prefix + YAML::Dump(yaml) + "\n" + suffix;
    // captured_output.append(prefix);
    // captured_output.append(YAML::Dump(yaml));
    // captured_output.append("\n");
    // captured_output.append(suffix);
  } else if (command.contains("replace")) {
    captured_output = prefix + std::regex_replace(captured_output, regex_search, command["replace"].get<std::string>()) + suffix;
  } else if (command.contains("match")) {
    std::smatch sm;
    std::string new_output      = prefix;
    inja::Environment local_env = inja_env; // Create copy and override `$()` function
    const auto match_string     = command["match"].get<std::string>();
    local_env.add_callback("reg", 1, [&](const inja::Arguments &args) {
      return sm[args[0]->get<int>()].str();
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

// blueprint_commands["template"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return template_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  try {
    std::string template_string;
    std::string template_filename;
    nlohmann::json data;
    if (command.is_string()) {
      captured_output = try_render(inja_env, command.get<std::string>(), data.is_null() ? project_summary : data);
      return { captured_output, 0 };
    }
    if (command.is_object()) {
      if (command.contains("data_file")) {
        std::filesystem::path data_filename = try_render(inja_env, command["data_file"].get<std::string>(), project_summary);
        if (data_filename.extension() == ".yaml" || data_filename.extension() == ".yml") {
          YAML::Node data_yaml = YAML::LoadFile(data_filename);
          if (!data_yaml.IsNull())
            data = data_yaml.as<nlohmann::json>();
        } else if (data_filename.extension() == ".json") {
          std::ifstream i(data_filename);
          i >> data;
        }
      } else if (command.contains("data")) {
        std::string data_string = try_render(inja_env, command["data"].get<std::string>(), project_summary);
        YAML::Node data_yaml    = YAML::Load(data_string);
        if (!data_yaml.IsNull())
          data = data_yaml.as<nlohmann::json>();
      }

      if (command.contains("template_file")) {
        template_filename = try_render(inja_env, command["template_file"].get<std::string>(), project_summary);
        captured_output   = try_render_file(inja_env, template_filename, data.is_null() ? project_summary : data);
        return { captured_output, 0 };
      }

      if (command.contains("template")) {
        template_string = command["template"].get<std::string>();
        captured_output = try_render(inja_env, template_string, data.is_null() ? project_summary : data);
        return { captured_output, 0 };
      }
    }

    spdlog::error("Inja template is invalid:\n'{}'", command.dump());
    return { "", -1 };
  } catch (std::exception &e) {
    spdlog::error("Failed to apply template: {}\n{}", command.dump(), e.what());
    return { "", -1 };
  }
  return { captured_output, 0 };
};

// Backwards compatibility with old 'inja' command
// blueprint_commands["inja"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return inja_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  return template_command(target, command, captured_output, project_summary, project_data, inja_env);
};

// blueprint_commands["save"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return save_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string save_filename;

  if (command.is_null())
    save_filename = target;
  else
    save_filename = try_render(inja_env, command.get<std::string>(), project_summary);

  // Special case to save data depenencies
  if (save_filename[0] == data_dependency_identifier) {
    if (!save_filename.starts_with(":/data/")) {
      spdlog::error("Data dependency pointer must start with '/data'");
      return { "", -1 };
    }
    auto pointer          = nlohmann::json::json_pointer{ save_filename.substr(6) };
    project_data[pointer] = captured_output;
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

// blueprint_commands["create_directory"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return create_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  if (!command.is_null()) {
    std::string filename = "";
    try {
      filename = command.get<std::string>();
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
};

// blueprint_commands["verify"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return verify_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string filename = command.get<std::string>();
  filename             = try_render(inja_env, filename, project_summary);
  if (fs::exists(filename)) {
    spdlog::info("{} exists", filename);
    return { captured_output, 0 };
  }

  spdlog::info("BAD!! {} doesn't exist", filename);
  return { "", -1 };
};

// blueprint_commands["rm"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return rm_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string filename = command.get<std::string>();
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

// blueprint_commands["rmdir"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return rmdir_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string path = command.get<std::string>();
  path             = try_render(inja_env, path, project_summary);
  // Put some checks here
  std::error_code ec;
  fs::remove_all(path, ec);
  if (!ec) {
    spdlog::error("'rmdir' command failed {}\n", ec.message());
  }
  return { captured_output, 0 };
};

// blueprint_commands["pack"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return pack_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::vector<std::byte> data_output;

  if (!command.contains("data")) {
    spdlog::error("'pack' command requires 'data'\n");
    return { "", -1 };
  }

  if (!command.contains("format")) {
    spdlog::error("'pack' command requires 'format'\n");
    return { "", -1 };
  }

  std::string format = command["format"].get<std::string>();
  format             = try_render(inja_env, format, project_summary);

  auto i = format.begin();
  for (auto d: command["data"]) {
    auto v       = try_render(inja_env, d.get<std::string>(), project_summary);
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

// blueprint_commands["copy"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return copy_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string destination;
  nlohmann::json source;
  try {

    destination = try_render(inja_env, command["destination"].get<std::string>(), project_summary);

    if (command.contains("source")) {
      source = command["source"];
    } else {
      if (command.contains("yaml_list")) {
        if (!command["yaml_list"].is_string()) {
          spdlog::error("'copy' command 'yaml_list' is not a string");
          return { "", -1 };
        }
        std::string list_yaml_string = try_render(inja_env, command["yaml_list"].get<std::string>(), project_summary);
        source                       = YAML::Load(list_yaml_string).as<nlohmann::json>();
      } else {
        spdlog::error("'copy' command does not have 'source' or 'yaml_list'");
        return { "", -1 };
      }
    }
    if (source.is_string()) {
      auto source_string = try_render(inja_env, source.get<std::string>(), project_summary);
      std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
    } else if (source.is_array()) {
      for (const auto &f: source) {
        auto source_string = try_render(inja_env, f.get<std::string>(), project_summary);
        std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
      }
    } else if (source.is_object()) {
      if (source.contains("folder_paths"))
        for (const auto &f: source["folder_paths"]) {
          auto source_string = try_render(inja_env, f.get<std::string>(), project_summary);
          auto dest          = destination + "/" + source_string;
          std::filesystem::create_directories(dest);
          std::filesystem::copy(source_string, dest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
        }
      if (source.contains("folders"))
        for (const auto &f: source["folders"]) {
          auto source_string = try_render(inja_env, f.get<std::string>(), project_summary);
          std::filesystem::copy(source_string, destination, std::filesystem::copy_options::recursive | std::filesystem::copy_options::update_existing);
        }
      if (source.contains("file_paths"))
        for (const auto &f: source["file_paths"]) {
          auto source_string         = try_render(inja_env, f.get<std::string>(), project_summary);
          std::filesystem::path dest = destination + "/" + source_string;
          std::filesystem::create_directories(dest.parent_path());
          std::filesystem::copy(source_string, dest, std::filesystem::copy_options::update_existing);
        }
      if (source.contains("files"))
        for (const auto &f: source["files"]) {
          auto source_string = try_render(inja_env, f.get<std::string>(), project_summary);
          std::filesystem::copy(source_string, destination, std::filesystem::copy_options::update_existing);
        }
    } else {
      spdlog::error("'copy' command missing 'source' or 'list' while processing {}", target);
      return { "", -1 };
    }
  } catch (std::exception &e) {
    spdlog::error("'copy' command failed while processing {}: '{}' -> '{}'\r\n{}", target, source.dump(), destination, e.what());
    return { "", -1 };
  }
  return { "", 0 };
};

// blueprint_commands["cat"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return cat_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  std::string filename = try_render(inja_env, command.get<std::string>(), project_summary);
  std::ifstream datafile;
  datafile.open(filename, std::ios_base::in | std::ios_base::binary);
  std::stringstream string_stream;
  string_stream << datafile.rdbuf();
  captured_output = string_stream.str();
  datafile.close();
  return { captured_output, 0 };
};

// blueprint_commands["new_project"] = [this](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
//   const auto project_string = command.get<std::string>();
//   yakka::project new_project(project_string, workspace);
//   new_project.init_project(project_string);
//   return { "", 0 };
// };

// blueprint_commands["as_json"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return as_json_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  const auto temp_json = nlohmann::json::parse(captured_output);
  return { temp_json.dump(2), 0 };
};

// blueprint_commands["as_yaml"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return as_yaml_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  const auto temp_yaml = YAML::Load(captured_output);
  return { YAML::Dump(temp_yaml), 0 };
};
// blueprint_commands["diff"] = [](std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env) -> yakka::process_return {
process_return diff_command(std::string target, const nlohmann::json &command, std::string captured_output, const nlohmann::json &project_summary, nlohmann::json &project_data, inja::Environment &inja_env)
{
  if (!command.is_object()) {
    spdlog::error("'diff' command invalid");
    return { "", -1 };
  }
  nlohmann::json left;
  nlohmann::json right;
  if (command.contains("left_file")) {
    const std::filesystem::path left_file = try_render(inja_env, command["left_file"].get<std::string>(), project_summary);
    std::ifstream ifs(left_file);
    left = nlohmann::json::parse(ifs);
  } else if (command.contains("left")) {
    left = try_render(inja_env, command["left"].get<std::string>(), project_summary);
  }

  if (command.contains("right_file")) {
    const std::filesystem::path right_file = try_render(inja_env, command["right_file"].get<std::string>(), project_summary);
    std::ifstream ifs(right_file);
    right = nlohmann::json::parse(ifs);
  } else if (command.contains("right")) {
    right = try_render(inja_env, command["right"].get<std::string>(), project_summary);
  }

  nlohmann::json patch = nlohmann::json::diff(left, right);
  return { patch.dump(), 0 };
};

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