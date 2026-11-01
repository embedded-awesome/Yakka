/**
 * @file blueprint_database.cpp
 * @brief Implements blueprint provider indexing and lookup routines.
 */

#include "blueprint_database.hpp"
#include "utilities.hpp"
#include "yakka.hpp"
#include "inja.hpp"
#include "glob/glob.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <stdexcept>

namespace yakka {
struct blueprint_database::render_context {
  struct environment_type : inja::Environment {
    void clear_temporaries()
    {
      // Successful renders already remove their nodes. Also remove any left by an exception.
      temp_data_tree.remove_children(temp_data_tree.rootref().id());
      temp_data_tree.clear_arena();
    }
  };

  explicit render_context(template_file_inputs &inputs)
  {
    add_common_template_commands(environment, &inputs);
    environment.add_callback("$", 1, [this](inja::Arguments &args, ryml::NodeRef additional_data) {
      const int32_t index = args[0].val<int32_t>().value();
      if (index >= 0 && static_cast<size_t>(index) < match->regex_matches.size())
        return additional_data["values"].append_child() << match->regex_matches[index];
      return ryml::NodeRef{};
    });
    environment.add_callback("curdir", 0, [this](inja::Arguments &, ryml::NodeRef additional_data) {
      return additional_data["values"].append_child() << match->blueprint->parent_path;
    });
    environment.add_callback("render", 1, [this](inja::Arguments &args, ryml::NodeRef additional_data) {
      return additional_data["values"].append_child() << render(args[0].val<std::string>().value());
    });
    environment.add_callback("select", 1, [](inja::Arguments &, ryml::NodeRef) {
      return ryml::NodeRef{};
    });
    environment.add_callback("aggregate", 1, [this](inja::Arguments &args, ryml::NodeRef additional_data) {
      const std::string path = args[0].val<std::string>().value();
      return aggregate_blueprint_data(environment, summary, c4::to_csubstr(path),
                                      additional_data["values"].append_child());
    });
  }

  std::string render(std::string_view text)
  {
    auto entry = templates.find(text);
    if (entry == templates.end()) {
      // Literal AST nodes view Template::content; do not move an SSO-backed parsed template.
      auto parsed = std::unique_ptr<inja::Template>(new inja::Template(environment.parse(text)));
      entry = templates.emplace(std::string(text), std::move(parsed)).first;
    }
    return environment.render(*entry->second, summary);
  }

  std::string try_render(c4::csubstr text)
  {
    try {
      return render(std::string_view(text.str, text.len));
    } catch (const std::exception &error) {
      spdlog::error("Template error: {}\n{}", text, error.what());
      return "";
    }
  }

  // A nested render() must see the same match, and failures must not retain its context.
  struct scope {
    scope(render_context &context, ryml::ConstNodeRef summary)
      : context(context), previous_match(context.match), previous_summary(context.summary)
    {
      context.summary = summary;
    }
    ~scope()
    {
      context.match = std::move(previous_match);
      context.summary = previous_summary;
      if (!context.match)
        context.environment.clear_temporaries();
    }
    render_context &context;
    std::shared_ptr<blueprint_match> previous_match;
    ryml::ConstNodeRef previous_summary;
  };

  environment_type environment;
  std::map<std::string, std::unique_ptr<inja::Template>, std::less<>> templates;
  std::shared_ptr<blueprint_match> match;
  ryml::ConstNodeRef summary;
};

blueprint_database::blueprint_database() : renderer(std::make_unique<render_context>(template_inputs))
{
  database.reserve_arena(16 * 1024 * 1024);               // Reserve 16MB to keep csubstr-backed arena storage stable during typical operations.
  database.add_flags(ryml::Tree::TREEF_NO_ARENA_REALLOC); // Forbid arena reallocations to ensure pointer stability of csubstrs.
  database.rootref() |= ryml::MAP;
  database["blueprints"] |= ryml::SEQ;
  database["matches"] |= ryml::SEQ;
  database["dependencies"] |= ryml::SEQ;
}

blueprint_database::~blueprint_database() = default;

/// @brief Executes find_match.
std::vector<std::shared_ptr<blueprint_match>> blueprint_database::find_match(ryml::csubstr target, ryml::ConstNodeRef project_summary)
{
  bool blueprint_match_found = false;

  std::vector<std::shared_ptr<blueprint_match>> result;
  struct matching_blueprint {
    c4::csubstr target;
    std::shared_ptr<blueprint> definition;
    std::vector<std::string> captures;
  };
  std::vector<matching_blueprint> matching_blueprints;

  const auto [first, last] = blueprints.equal_range(target);
  for (auto blueprint = first; blueprint != last; ++blueprint) {
    if (blueprint->second->regex.has_value())
      continue;
    matching_blueprints.push_back({ blueprint->first, blueprint->second, { ryml_string(target) } });
  }

  if (!regex_blueprints.empty()) {
    const std::string target_string = ryml_string(target);
    for (const auto &blueprint: regex_blueprints) {
      std::vector<std::string> captures;
      if (blueprint->match_regex(target_string, captures))
        matching_blueprints.push_back({ blueprint->target, blueprint, std::move(captures) });
    }
  }

  std::sort(matching_blueprints.begin(), matching_blueprints.end(), [](const auto &left, const auto &right) {
    if (left.target != right.target)
      return left.target < right.target;
    return left.definition->insertion_order < right.definition->insertion_order;
  });

  render_context::scope render_scope(*renderer, project_summary);
  for (const auto &candidate: matching_blueprints) {
    const auto &definition = candidate.definition;
    auto match = std::make_shared<blueprint_match>();

    for (const auto &capture: candidate.captures) {
      auto match_node = database["matches"].append_child() << capture;
      match->regex_matches.push_back(match_node.val());
    }

    // Found a match. Create a blueprint match object
    blueprint_match_found = true;
    match->blueprint      = definition;

    renderer->match = match;

    auto render_project_argument = [&](c4::csubstr argument) {
      const auto value = renderer->render(std::string_view(argument.str, argument.len));
      if (value.empty())
        throw std::invalid_argument("Project dependency arguments must not be empty");
      auto stored = database["dependencies"].append_child() << value;
      return stored.val();
    };
    auto add_project = [&](ryml::ConstNodeRef arguments) {
      if (!arguments.is_seq() || arguments.num_children() == 0)
        throw std::invalid_argument("Project dependency must be a nonempty array of build arguments");
      std::vector<c4::csubstr> rendered;
      for (const auto argument: arguments.children()) {
        if (!argument.is_val() || argument.val_is_null())
          throw std::invalid_argument("Project dependency arguments must be strings");
        rendered.push_back(render_project_argument(argument.val()));
      }
      match->project_dependencies.push_back(std::move(rendered));
    };
    auto add_dependency = [&](ryml::ConstNodeRef node) {
      if (node.is_map() && node.has_child("project")) {
        add_project(node["project"]);
      } else if (node.is_val() && !node.val_is_null()) {
        auto value = node.val();
        auto dependency = database["dependencies"].append_child() <<
          (value.begins_with("./") ? value.sub(value.first_not_of('/', 2)) : value);
        match->dependencies.push_back(dependency.val());
      } else {
        throw std::invalid_argument("Dependency must be a string or a project object");
      }
    };

    // Run template engine on dependencies
    for (auto d: definition->dependencies) {
      switch (d.type) {
        case blueprint::dependency::PROJECT_DEPENDENCY: {
          std::vector<c4::csubstr> arguments;
          for (const auto argument: d.arguments)
            arguments.push_back(render_project_argument(argument));
          match->project_dependencies.push_back(std::move(arguments));
          continue;
        }
        case blueprint::dependency::DEPENDENCY_FILE_DEPENDENCY: {
          const std::string generated_dependency_file = renderer->try_render(d.name);
          auto stored_file                            = database["dependencies"].append_child() << generated_dependency_file;
          match->dependency_files.push_back(stored_file.val());
          if (!expand_dependency_files)
            continue;
          auto dependencies = parse_gcc_dependency_file(generated_dependency_file);
          match->dependencies.insert(std::end(match->dependencies), std::begin(dependencies), std::end(dependencies));
          continue;
        }
        case blueprint::dependency::DATA_DEPENDENCY: {
          std::string data_name = renderer->try_render(d.name);
          if (data_name.empty()) {
            spdlog::error("Empty data dependency for '{}'", target);
            return {};
          }
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
        generated_depend = renderer->render(std::string_view(d.name.data(), d.name.size()));
      } catch (std::exception &e) {
        spdlog::error("Error evaluating dependency for {}\r\nCouldn't apply template: '{}'\n{}", definition->target, d.name, e.what());
        return result;
      }

      if (generated_depend.empty())
        continue;

      // Structured template output can be a list of dependencies or a project object.
      const auto first = generated_depend.find_first_not_of(" \t\r\n");
      if (first != std::string::npos && (generated_depend[first] == '[' || generated_depend[first] == '{' ||
                                        generated_depend.compare(first, 8, "project:") == 0)) {
        try {
          const auto generated_tree = ryml_parse_yaml(c4::to_csubstr(generated_depend));
          const auto generated_node = generated_tree.crootref();
          if (generated_node.is_seq())
            for (const auto entry: generated_node.children())
              add_dependency(entry);
          else
            add_dependency(generated_node);
        } catch (const std::exception &e) {
          throw std::runtime_error("Invalid dependency for '" + ryml_string(target) + "': " + e.what());
        }
      } else {
        auto dependency = database["dependencies"].append_child() << (generated_depend.starts_with("./") ? generated_depend.substr(generated_depend.find_first_not_of("/", 2)) : generated_depend);
        match->dependencies.push_back(dependency.val());
      }
    }

    result.push_back(match);
  }

  if (!blueprint_match_found) {
    if (!target.empty() && !fs::exists(ryml_string(target)) && target[0] != yakka::data_dependency_identifier)
      spdlog::info("No blueprint for '{}'", target);
  }
  return result;
}

/// @brief Executes load.

void blueprint_database::load(const std::filesystem::path filename)
{
  auto loaded = ryml_load_file(filename);
  if (!loaded) {
    spdlog::error("Failed to load blueprints '{}': {}", filename.string(), loaded.error().message());
    return;
  }
  if (!loaded->crootref().is_map()) {
    spdlog::error("Blueprint database '{}' must be a map", filename.string());
    return;
  }
  blueprints.clear();
  renderer->templates.clear();
  regex_blueprints.clear();
  next_blueprint_order = 0;
  database.clear();
  database.rootref() |= ryml::MAP;
  database["blueprints"] |= ryml::SEQ;
  database["matches"] |= ryml::SEQ;
  database["dependencies"] |= ryml::SEQ;
  loaded_blueprints = std::move(*loaded);
  for (const auto &blueprint_group: loaded_blueprints.crootref().children()) {
    auto load_blueprint = [&](ryml::ConstNodeRef blueprint_node) {
      if (!blueprint_node.is_map())
        return;

      auto target = blueprint_node.has_child("target") ? blueprint_node["target"].val() : blueprint_group.key();
      auto new_blueprint = std::make_shared<blueprint>(target, blueprint_node, blueprint_node.has_child("parent_path") ? blueprint_node["parent_path"].val() : c4::csubstr{});
      if (new_blueprint->regex.has_value() && new_blueprint->regex->empty())
      {
        new_blueprint->regex.reset();
        new_blueprint->compiled_regex.reset();
      }
      if (blueprint_node.has_child("requirements")) {
        for (const auto requirement: blueprint_node["requirements"].children())
          new_blueprint->requirements.push_back(requirement.val());
      }
      if (blueprint_node.has_child("task_group"))
        new_blueprint->task_group = blueprint_node["task_group"].val();

      if (blueprint_node.has_child("dependencies") && blueprint_node["dependencies"].is_seq()) {
        new_blueprint->dependencies.clear();
        for (const auto &dependency_node: blueprint_node["dependencies"].children()) {
          if (!dependency_node.is_map() || !dependency_node.has_child("type") || !dependency_node.has_child("name"))
            continue;

          auto dependency_type = dependency_node["type"].val<int>().value();
          new_blueprint->dependencies.push_back({ static_cast<blueprint::dependency::dependency_type>(dependency_type), dependency_node["name"].val() });
          if (dependency_node.has_child("arguments"))
            for (const auto argument: dependency_node["arguments"].children())
              new_blueprint->dependencies.back().arguments.push_back(argument.val());
        }
      }

      new_blueprint->insertion_order = next_blueprint_order++;
      blueprints.insert({ target, new_blueprint });
      if (new_blueprint->regex.has_value())
        regex_blueprints.push_back(new_blueprint);
    };

    if (blueprint_group.is_seq()) {
      for (const auto &blueprint_node: blueprint_group.children())
        load_blueprint(blueprint_node);
    } else {
      load_blueprint(blueprint_group);
    }
  }
}

/// @brief Executes save.

void blueprint_database::save(const std::filesystem::path filename)
{
  ryml::Tree output;

  output.reserve_arena(2 * 1024 * 1024); // Reserve 2MB for the arena to avoid reallocations during saving, which would invalidate all node references
  auto output_root = output.rootref();
  output_root |= ryml::MAP;

  for (const auto &bp: blueprints) {
    ryml::NodeRef blueprint_list;
    if (output_root.has_child(bp.first)) {
      blueprint_list = output_root[bp.first];
    } else {
      blueprint_list = output_root.append_child() << ryml::Key(bp.first);
      blueprint_list |= ryml::SEQ;
    }

    auto blueprint = blueprint_list.append_child();
    blueprint |= ryml::MAP;
    blueprint["target"] << bp.second->target;
    blueprint["regex"] << bp.second->regex.value_or("");
    blueprint["parent_path"] << bp.second->parent_path;
    blueprint["task_group"] << bp.second->task_group;
    auto requirements = blueprint.append_child() << ryml::Key("requirements");
    requirements |= ryml::SEQ;
    for (const auto requirement: bp.second->requirements)
      requirements.append_child() << requirement;
    if (bp.second->process.valid()) {
      auto process = blueprint.append_child() << ryml::Key("process");
      merge_nodes(process, bp.second->process);
    }

    auto dependencies = blueprint.append_child() << ryml::Key("dependencies");
    dependencies |= ryml::SEQ;
    for (const auto &dep: bp.second->dependencies) {
      auto dependency = dependencies.append_child();
      dependency |= ryml::MAP;
      dependency["type"] << dep.type;
      dependency["name"] << dep.name;
      if (dep.type == blueprint::dependency::PROJECT_DEPENDENCY) {
        dependency["arguments"] |= ryml::SEQ;
        for (const auto &argument: dep.arguments)
          dependency["arguments"].append_child() << argument;
      }
    }
  }

  write_file_if_changed(filename, ryml::emitrs_json<std::string>(output));
}

/**
 * @brief Parses dependency files as output by GCC or Clang generating a vector of filenames as found in the named file
 *
 * @param filename  Name of the dependency file. Typically ending in '.d'

/// @brief Executes parse_gcc_dependency_file.

 * @return std::vector<ryml::csubstr>  Vector of files specified as dependencies
 */
std::vector<ryml::csubstr> blueprint_database::parse_gcc_dependency_file(const std::string &filename)
{
  dependency_files.insert(filename);
  std::vector<ryml::csubstr> dependencies;
  std::ifstream infile(filename);

  if (!infile.is_open())
    return {};

  std::string line;

  // Ignore the target, retaining dependencies that occur on the same line.
  while (std::getline(infile, line) && line.find(':') == std::string::npos) {}

  const auto separator = line.find(':');
  if (separator == std::string::npos)
    return {};
  std::string content = line.substr(separator + 1);
  content.push_back('\n');
  content.append(std::istreambuf_iterator<char>(infile), {});
  std::string token;
  const auto append = [&]() {
    if (token.empty())
      return;
    auto dependency = database["dependencies"].append_child() << (token.starts_with("./") ? token.substr(2) : token);
    dependencies.push_back(dependency.val());
    token.clear();
  };
  for (size_t i = 0; i < content.size(); ++i) {
    const char character = content[i];
    if (character == '\\' && i + 1 < content.size()) {
      if (content[i + 1] == '\r' || content[i + 1] == '\n') {
        ++i;
        if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n')
          ++i;
      } else if (content[i + 1] == ' ' || content[i + 1] == '\\') {
        token.push_back(content[++i]);
      } else {
        token.push_back(character);
      }
    } else if (character == '\n' || character == '\r') {
      append();
      break;
    } else if (std::isspace(static_cast<unsigned char>(character))) {
      append();
    } else {
      token.push_back(character);
    }
  }
  append();

  return dependencies;
}

/// @brief Executes create_blueprint.

void blueprint_database::create_blueprint(const std::string &target, ryml::ConstNodeRef blueprint_data, c4::csubstr parent_path)
{
  auto blueprint_target     = database["blueprints"].append_child() << target;

  auto new_blueprint = std::make_shared<blueprint>(blueprint_target.val(), blueprint_data, parent_path);
  new_blueprint->insertion_order = next_blueprint_order++;
  blueprints.insert({ blueprint_target.val(), new_blueprint });
  if (new_blueprint->regex.has_value())
    regex_blueprints.push_back(new_blueprint);
}
} // namespace yakka
