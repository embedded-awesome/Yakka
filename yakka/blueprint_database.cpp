#include "blueprint_database.hpp"
#include "utilities.hpp"
#include "yakka.hpp"
#include "inja.hpp"
#include "glob/glob.h"
#include "spdlog/spdlog.h"
#include <regex>

namespace yakka {
std::vector<std::shared_ptr<blueprint_match>> blueprint_database::find_match(ryml::csubstr target, ryml::ConstNodeRef project_summary)
{
  bool blueprint_match_found = false;
  std::vector<std::shared_ptr<blueprint_match>> result;

  for (const auto &blueprint: blueprints) {
    auto match = std::make_shared<blueprint_match>();

    // Check if rule is a regex, otherwise do a string comparison
    if (blueprint.second->regex.has_value()) {
      std::smatch s;
      std::string target_str = ryml_string(target);
      if (!std::regex_match(target_str, s, std::regex{ ryml_string(blueprint.first) }))
        continue;
      
      // arg_count starts at 0 as the first match is the entire string
      for (auto &regex_match: s) {
        auto match_node = database["matches"].append_child() << regex_match.str();
        match->regex_matches.push_back(match_node.val());
      }
    } else {
      if (target != blueprint.first)
        continue;

      match->regex_matches.push_back(target);
    }

    // Found a match. Create a blueprint match object
    blueprint_match_found = true;
    match->blueprint      = blueprint.second;

    inja::Environment local_inja_env;

    add_common_template_commands(local_inja_env);

    local_inja_env.add_callback("$", 1, [&match](inja::Arguments &args, ryml::Tree &additional_data) {
      const int32_t index = args[0].val<int32_t>().value();
      if (index < match->regex_matches.size())
        return additional_data.rootref().append_child() << match->regex_matches[index];

      return ryml::NodeRef{};
    });
    local_inja_env.add_callback("curdir", 0, [&match](inja::Arguments &args, ryml::Tree &additional_data) {
      return additional_data.rootref().append_child() << match->blueprint->parent_path;
    });
    local_inja_env.add_callback("render", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
      return additional_data.rootref().append_child() << local_inja_env.render(args[0].val<std::string>().value(), project_summary);
    });
    local_inja_env.add_callback("select", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
      // TODO
      return ryml::NodeRef{};
      // inja::ryml_json choice;
      // for (const auto &option: args.at(0)->items()) {
      //   const auto option_type = option.key();
      //   const auto option_name = option.value();
      //   if ((option_type == "feature" && project_summary["features"].has_child(option_name)) || (option_type == "component" && project_summary["components"].has_child(option_name))) {
      //     assert(choice.is_null());
      //     choice = option_name;
      //   }
      // }
      // return choice;
    });
    local_inja_env.add_callback("aggregate", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
      auto aggregate = additional_data.rootref().append_child() << ryml::MAP;
      auto path = ryml::Pointer{args[0].val()};

      // Loop through components, check if object path exists, if so add it to the aggregate
      for (const auto &child: project_summary["components"].children()) {
        // auto c_value = child.val();
        if (!child[path].valid())
          continue;

        auto v = child[path];
        if (v.is_map())
          for (auto i: v.children())
            aggregate[i.key()] = i.val(); //local_inja_env.render(i.second.as<std::string>(), this->project_summary);
        else if (v.is_seq())
          for (const auto &i: v.children())
            aggregate.append_child() << local_inja_env.render(i.val<std::string>().value(), project_summary);
        else
          aggregate.append_child() << local_inja_env.render(v.val<std::string>().value(), project_summary);
      }

      // Check project data
      if (project_summary["data"][path].valid()) {
        auto v = project_summary["data"][path];
        if (v.is_map())
          for (auto i: v.children())
            aggregate[i.key()] = i.val();
        else if (v.is_seq())
          for (const auto &i: v.children())
            aggregate.append_child() << local_inja_env.render(i.val<std::string>().value(), project_summary);
        else
          aggregate.append_child() << local_inja_env.render(v.val<std::string>().value(), project_summary);
      }
      return aggregate;
    });

    // Run template engine on dependencies
    for (auto d: blueprint.second->dependencies) {
      switch (d.type) {
        case blueprint::dependency::DEPENDENCY_FILE_DEPENDENCY: {
          const std::string generated_dependency_file = yakka::try_render(local_inja_env, d.name, project_summary);
          auto dependencies                           = parse_gcc_dependency_file(generated_dependency_file);
          match->dependencies.insert(std::end(match->dependencies), std::begin(dependencies), std::end(dependencies));
          continue;
        }
        case blueprint::dependency::DATA_DEPENDENCY: {
          std::string data_name = yakka::try_render(local_inja_env, d.name, project_summary);
          if (data_name.front() != yakka::data_dependency_identifier)
            data_name.insert(0, 1, yakka::data_dependency_identifier);
          auto dependency = database["dependencies"].append_child() << data_name;
          match->dependencies.push_back(dependency.val());
          continue;
        }
        default:
          break;
      }

      // Generate full dependency string by applying template engine
      std::string generated_depend;
      try {
        generated_depend = local_inja_env.render(std::string_view(d.name.data(), d.name.size()), project_summary);
      } catch (std::exception &e) {
        spdlog::error("Error evaluating dependency for {}\r\nCouldn't apply template: '{}'\n{}", ryml_string(blueprint.first), ryml_string(d.name), e.what());
        return result;
      }

      // Check if the input was a YAML array construct
      if (generated_depend.front() == '[' && generated_depend.back() == ']') {
        // Load the generated dependency string as YAML and push each item individually
        try {
          auto generated_node = YAML::Load(generated_depend);
          for (auto i: generated_node) {
            auto temp = i.Scalar();
            auto dependency = database["dependencies"].append_child() << (temp.starts_with("./") ? temp.substr(temp.find_first_not_of("/", 2)) : temp);
            match->dependencies.push_back(dependency.val());
          }
        } catch (std::exception &e) {
          std::cerr << "Failed to parse dependency: " << ryml_string(d.name) << "\n";
        }
      } else {
        auto dependency = database["dependencies"].append_child() << (generated_depend.starts_with("./") ? generated_depend.substr(generated_depend.find_first_not_of("/", 2)) : generated_depend);
        match->dependencies.push_back(dependency.val());
      }
    }

    result.push_back(match);
  }

  if (!blueprint_match_found) {
    if (!fs::exists(ryml_string(target)) && target[0] != yakka::data_dependency_identifier)
      spdlog::info("No blueprint for '{}'", ryml_string(target));
  }
  return result;
}

void blueprint_database::load(const std::filesystem::path filename)
{
  database = ryml_load_file(filename).value();

  // Generate blueprints from database
}

void blueprint_database::save(const std::filesystem::path filename)
{
  ryml::Tree output;
  auto output_root = output.rootref() << ryml::MAP;

  for (const auto &bp: blueprints) {
    auto blueprint = output_root.append_child() << ryml::MAP << ryml::Key(bp.first);
    blueprint["target"]      << bp.second->target;
    blueprint["regex"]       << bp.second->regex.value_or("");
    blueprint["parent_path"] << bp.second->parent_path;

    auto dependencies = blueprint.append_child() << ryml::SEQ << ryml::Key("dependencies");
    for (const auto &dep: bp.second->dependencies) {
      auto dependency = dependencies.append_child() << ryml::MAP;
      dependency["type"] << dep.type;
      dependency["name"] << dep.name;
    }
  }

  std::ofstream file(filename);
  file << ryml::emitrs_json<std::string>(output);
}

/**
 * @brief Parses dependency files as output by GCC or Clang generating a vector of filenames as found in the named file
 *
 * @param filename  Name of the dependency file. Typically ending in '.d'
 * @return std::vector<ryml::csubstr>  Vector of files specified as dependencies
 */
std::vector<ryml::csubstr> blueprint_database::parse_gcc_dependency_file(const std::string &filename)
{
  std::vector<ryml::csubstr> dependencies;
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
    if (line.rfind("./", 0) == 0) {
      auto dependency = database["dependencies"].append_child() << line.substr(2);
      dependencies.push_back(dependency.val());
    } else {
      auto dependency = database["dependencies"].append_child() << line;
      dependencies.push_back(dependency.val());
    }
  }

  return dependencies;
}
} // namespace yakka
