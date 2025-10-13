#include "yakka.hpp"
#include "yakka_project.hpp"
#include "yakka_schema.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "glob/glob.h"
#include <nlohmann/json-schema.hpp>
#include <fstream>
#include <chrono>
#include <thread>
#include <string>
#include <charconv>

using namespace std;

namespace yakka {
using namespace std::chrono_literals;

project::project(const std::string project_name, yakka::workspace &workspace) : project_name(project_name), yakka_home_directory("/.yakka"), project_directory("."), workspace(workspace)
{
  // abort_build      = false;
  project_has_slcc = false;
  current_state    = yakka::project::state::PROJECT_VALID;
  component_flags  = component_database::flag::ALL_COMPONENTS;

  output_path          = yakka::default_output_directory + project_name;
  project_summary_file = output_path / yakka::project_summary_filename;
  project_file         = project_name + ".yakka";

  add_common_template_commands(inja_environment);
}

project::~project()
{
}

void project::set_project_directory(const std::string path)
{
  project_directory = path;
}

void project::process_build_string(const std::string build_string)
{
  // When C++20 ranges are available
  // for (const auto word: std::views::split(build_string, " ")) {

  std::stringstream ss(build_string);
  std::string word;
  while (std::getline(ss, word, ' ')) {
    // Identify features, commands, and components
    if (word.front() == '+')
      this->initial_features.push_back(word.substr(1));
    else if (word.back() == '!')
      this->commands.insert(word.substr(0, word.size() - 1));
    else
      this->initial_components.push_back(word);
  }
}

void project::init_project(const std::string build_string)
{
  process_build_string(build_string);

  for (const auto &c: initial_components)
    unprocessed_components.insert(c);
  for (const auto &f: initial_features)
    unprocessed_features.insert(f);
  init_project();
}

void project::init_project(std::vector<std::string> components, std::vector<std::string> features, std::unordered_set<std::string> commands)
{
  initial_features = features;

  for (const auto &c: components) {
    unprocessed_components.insert(c);
    initial_components.push_back(c);
  }
  for (const auto &f: features) {
    unprocessed_features.insert(f);
    initial_features.push_back(f);
  }
  this->commands = commands;
  
  init_project();
}

void project::init_project()
{
  if (fs::exists(project_summary_file)) {
    project_summary_last_modified = fs::last_write_time(project_summary_file);
    std::ifstream i(project_summary_file);
    i >> project_summary;
    i.close();

    // Fill required_features with features from project summary
    for (auto &f: project_summary["features"])
      required_features.insert(f.get<std::string>());

    project_summary["choices"] = {};
    update_summary();
  } else
    fs::create_directories(output_path);

  // Check if there is a project file
  if (fs::exists(project_file)) {
    YAML::Node node = YAML::LoadFile(project_file);
    // Merge data from the project file
    json_node_merge("/data"_json_pointer, project_summary, node.as<nlohmann::json>());
  }
}

void project::process_requirements(std::shared_ptr<yakka::component> component, nlohmann::json child_node)
{
  // Merge the feature values into the parent component
  json_node_merge(""_json_pointer, component->json, child_node);

  // Process required components
  if (child_node.contains("/requires/components"_json_pointer)) {
    // Add the item/s to the new_component list
    if (child_node["requires"]["components"].is_string())
      unprocessed_components.insert(child_node["requires"]["components"].get<std::string>());
    else if (child_node["requires"]["components"].is_array())
      for (const auto &i: child_node["requires"]["components"])
        unprocessed_components.insert(i.get<std::string>());
    else
      spdlog::error("Node '{}' has invalid 'requires'", child_node["requires"].get<std::string>());
  }

  // Process required features
  if (child_node.contains("/requires/features"_json_pointer)) {
    const auto node = child_node["requires"]["features"];
    // Add the item/s to the new_features list
    if (node.is_string()) {
      const auto feature = node.is_string() ? node.get<std::string>() : node["name"].get<std::string>();
      if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
        slc_required.insert(feature);
      unprocessed_features.insert(feature);
      // If the feature has recommendations, add them to the feature_recommendations map
      if (node.is_object() && node.contains("recommends"))
        feature_recommendations.insert({ feature, node["recommends"] });
    } else if (node.is_array())
      for (const auto &i: node) {
        const auto feature = i.is_string() ? i.get<std::string>() : i["name"].get<std::string>();
        if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
          slc_required.insert(feature);
        unprocessed_features.insert(feature);
        // If the feature has recommendations, add them to the feature_recommendations map
        if (i.is_object() && i.contains("recommends"))
          feature_recommendations.insert({ feature, i["recommends"] });
      }
    else
      spdlog::error("Node '{}' has invalid 'requires'", child_node["requires"].get<std::string>());
  }

  // Process provided features
  if (child_node.contains("/provides/features"_json_pointer)) {
    auto child_node_provides = child_node["provides"]["features"];
    if (child_node_provides.is_string()) {
      const auto feature = child_node_provides.get<std::string>();
      if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
        slc_provided.insert(feature);
      // unprocessed_features.insert(feature);
      provided_features.insert(feature);
    } else if (child_node_provides.is_array())
      for (const auto &i: child_node_provides) {
        const auto feature = i.get<std::string>();
        if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
          slc_provided.insert(feature);
        // unprocessed_features.insert(feature);
        provided_features.insert(feature);
      }
  }

  // Process choices
  for (const auto &[choice_name, choice]: child_node["choices"].items()) {
    if (!project_summary["choices"].contains(choice_name)) {
      unprocessed_choices.insert(choice_name);
      project_summary["choices"][choice_name]           = choice;
      project_summary["choices"][choice_name]["parent"] = component->json["name"].get<std::string>();
    }
  }

  // Process supported components
  if (child_node.contains("/supports/components"_json_pointer)) {
    for (const auto &c: required_components)
      if (child_node["supports"]["components"].contains(c)) {
        spdlog::info("Processing component '{}' in {}", c, component->json["name"].get<std::string>());
        process_requirements(component, child_node["supports"]["components"][c]);
      }
  }

  // Process supported features
  if (child_node.contains("/supports/features"_json_pointer)) {
    for (const auto &f: required_features)
      if (child_node["supports"]["features"].contains(f)) {
        spdlog::info("Processing feature '{}' in {}", f, component->json["name"].get<std::string>());
        process_requirements(component, child_node["supports"]["features"][f]);
      }
  }
}

void project::update_summary()
{
  // Check if any component files have been modified
  for (const auto &[name, value]: project_summary["components"].items()) {
    if (value.is_null())
      continue;
    if (!value.contains("yakka_file")) {
      spdlog::error("Project summary for component '{}' is missing 'yakka_file' entry", name);
      project_summary["components"].erase(name);
      unprocessed_components.insert(name);
      continue;
    }

    auto yakka_file = value["yakka_file"].get<std::string>();
    if (!std::filesystem::exists(yakka_file) || std::filesystem::last_write_time(yakka_file) > project_summary_last_modified) {
      // If so, move existing data to previous summary
      previous_summary["components"][name] = value; // TODO: Verify this is correct way to do this efficiently
      project_summary["components"][name]  = {};
      unprocessed_components.insert(name);
    } else {
      // Previous summary should point to the same object
      previous_summary["components"][name] = value;
    }
  }
  previous_summary["data"] = project_summary["data"];
}

bool project::add_component(const std::string &component_name, component_database::flag flags)
{
  // Convert string to id
  const auto component_id = yakka::component_dotname_to_id(component_name);

  // Check if component has been replaced
  if (replacements.contains(component_id)) {
    spdlog::info("Skipping {}. Being replaced by {}", component_id, replacements[component_id]);
    unprocessed_components.insert(replacements[component_id]);
    return false;
  }

  // Find the component in the project component database
  auto component_location = workspace.find_component(component_id, flags);
  if (!component_location) {
    // spdlog::info("{}: Couldn't find it", c);
    unknown_components.insert(component_id);
    return false;
  }

  // Add component to the required list and continue if this is not a new component
  // Insert component and continue if this is not new
  if (required_components.insert(component_id).second == false)
    return false;

  auto [component_path, package_path]             = component_location.value();
  std::shared_ptr<yakka::component> new_component = std::make_shared<yakka::component>();
  if (new_component->parse_file(component_path, package_path) == yakka::yakka_status::SUCCESS) {
    components.push_back(new_component);
  } else {
    current_state = project::state::PROJECT_HAS_INVALID_COMPONENT;
    return false;
  }

  // Add special processing of SLC related files and data
  if (new_component->type == yakka::component::YAKKA_FILE) {
    if (this->project_has_slcc)
      for (const auto &f: new_component->json["requires"]["slc"])
        slc_required.insert(f.get<std::string>());
  } else if (new_component->type == yakka::component::SLCC_FILE) {
    project_has_slcc = true;
    unprocessed_components.insert("jinja");
    for (const auto &f: new_component->json["requires"]["features"])
      slc_required.insert(f.get<std::string>());
    for (const auto &f: new_component->json["provides"]["features"])
      slc_provided.insert(f.get<std::string>());
    for (const auto &r: new_component->json["recommends"]) {
      auto id        = r["id"].get<std::string>();
      auto start_pos = id.find('%');
      auto end_pos   = id.rfind('%');
      if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos)
        id.erase(start_pos, end_pos - start_pos + 1);
      slc_recommended.insert({ id, r });
    }
    for (const auto &[key, instance_list]: new_component->json["instances"].items())
      for (const auto &i: instance_list)
        this->instances.insert({ key, i.get<std::string>() });
    // Extract config overrides
    for (const auto &c: new_component->json["config_file"])
      if (c.contains("override")) {
        slc_overrides.insert({ c["override"]["file_id"].get<std::string>(), new_component });
      }

  } else if (new_component->type == yakka::component::SLCP_FILE) {
    unprocessed_components.insert("jinja");
    for (const auto &f: new_component->json["requires"]["features"])
      slc_required.insert(f.get<std::string>());
    for (const auto &r: new_component->json["recommends"]) {
      auto id        = r["id"].get<std::string>();
      auto start_pos = id.find('%');
      auto end_pos   = id.rfind('%');
      if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos)
        id.erase(start_pos, end_pos - start_pos + 1);
      slc_recommended.insert({ id, r });
    }
    for (const auto &[key, instance_list]: new_component->json["instances"].items())
      for (const auto &i: instance_list)
        this->instances.insert({ key, i.get<std::string>() });
  }

  // Add all the required components into the unprocessed list
  if (new_component->json.contains("/requires/components"_json_pointer))
    for (const auto &r: new_component->json["requires"]["components"]) {
      unprocessed_components.insert(r.get<std::string>());
      if (r.contains("instance")) {
        for (const auto &i: r["instance"])
          instances.insert({ r.get<std::string>(), i.get<std::string>() });
      }
    }

  // Add all the required features into the unprocessed list
  if (new_component->json.contains("/requires/features"_json_pointer))
    for (const auto &f: new_component->json["requires"]["features"]) {
      if (f.is_string())
        unprocessed_features.insert(f.get<std::string>());
      else {
        unprocessed_features.insert(f["name"].get<std::string>());
        if (f.contains("recommends")) {
          feature_recommendations.insert({ f["name"].get<std::string>(), f["recommends"] });
        }
      }
    }

  // Add all the provided features into the unprocessed list
  if (new_component->json.contains("/provides/features"_json_pointer))
    for (const auto &f: new_component->json["provides"]["features"]) {
      // unprocessed_features.insert(f.get<std::string>());
      provided_features.insert(f.get<std::string>());
    }

  // Add all the component choices to the global choice list
  if (new_component->json.contains("choices"))
    for (auto &[choice_name, value]: new_component->json["choices"].items()) {
      if (!project_summary["choices"].contains(choice_name)) {
        unprocessed_choices.insert(choice_name);
        project_summary["choices"][choice_name]           = value;
        project_summary["choices"][choice_name]["parent"] = new_component->id;
      }
    }

  if (new_component->json.contains("/replaces/component"_json_pointer)) {
    const auto &replaced = new_component->json["replaces"]["component"].get<std::string>();

    if (replacements.contains(replaced)) {
      if (replacements[replaced] != component_id) {
        spdlog::error("Multiple components replacing {}", replaced);
        current_state = project::state::PROJECT_HAS_MULTIPLE_REPLACEMENTS;
        return false;
      }
    } else {
      spdlog::info("{} replaces {}", component_id, replaced);
      unprocessed_replacements.insert({ replaced, component_id });
    }
  }

  // Process all the currently required features. Note new feature will be processed in the features pass
  if (new_component->json.contains("/supports/features"_json_pointer)) {
    for (auto &f: required_features)
      if (new_component->json["supports"]["features"].contains(f)) {
        spdlog::info("Processing required feature '{}' in {}", f, component_id);
        process_requirements(new_component, new_component->json["supports"]["features"][f]);
      }
  }
  if (new_component->json.contains("/supports/components"_json_pointer)) {
    // Process the new components support for all the currently required components
    for (auto &c: required_components)
      if (new_component->json["supports"]["components"].contains(c)) {
        spdlog::info("Processing required component '{}' in {}", c, component_id);
        process_requirements(new_component, new_component->json["supports"]["components"][c]);
      }
  }

  // Process all the existing components support for the new component
  for (auto &c: components)
    if (c->json.contains("/supports/components"_json_pointer / component_id)) {
      // if (c->json.contains("supports") && c->json["supports"].contains("components") && c->json["supports"]["components"].contains(component_id)) {
      spdlog::info("Processing component '{}' in {}", component_id, c->json["name"].get<std::string>());
      process_requirements(c, c->json["supports"]["components"][component_id]);
    }

  return true;
}

bool project::add_feature(const std::string &feature_name)
{
  // Insert feature and continue if this is not new
  if (required_features.insert(feature_name).second == false)
    return false;

  if (!provided_features.contains(feature_name)) {
    unprovided_features.insert(feature_name);
  }

  // Process the feature "supports" for each existing component
  for (auto &c: components)
    if (c->json.contains("/supports/features"_json_pointer / feature_name)) {
      // if (c->json.contains("supports") && c->json["supports"].contains("features") && c->json["supports"]["features"].contains(f)) {
      spdlog::info("Processing feature '{}' in {}", feature_name, c->json["name"].get<std::string>());
      process_requirements(c, c->json["supports"]["features"][feature_name]);
    }

  return true;
}

/**
 * @brief Processes all the @ref unprocessed_components and @ref unprocessed_features, adding items to @ref unknown_components if they are not in the component database
 *        It is assumed the caller will process the @ref unknown_components before adding them back to @ref unprocessed_component and calling this again.
 * @return project::state
 */
project::state project::evaluate_dependencies()
{
  //project_has_slcc = false;

  // Start processing all the required components and features
  while (!unprocessed_components.empty() || !unprocessed_features.empty() || !slc_required.empty()) {
    // Loop through the list of unprocessed components.
    // Note: Items will be added to unprocessed_components during processing
    component_list_t temp_component_list = std::move(unprocessed_components);
    for (const auto &i: temp_component_list) {
      // Try add the component
      if (!add_component(i, component_flags)) {
        if (current_state != yakka::project::state::PROJECT_VALID)
          return current_state;
      }
    }

    // Process all the new features
    // Note: Items will be added to unprocessed_features during processing
    feature_list_t temp_feature_list = std::move(unprocessed_features);
    for (const auto &f: temp_feature_list) {
      add_feature(f);
    }

    // Check if we have finished but we have unprocessed choices
    if (unprocessed_components.empty() && unprocessed_features.empty() && !unprocessed_choices.empty()) {
      for (const auto &c: unprocessed_choices) {
        const auto &choice = project_summary["choices"][c];
        int matches        = 0;
        if (choice.contains("features"))
          matches = std::count_if(choice["features"].begin(), choice["features"].end(), [&](const nlohmann::json &j) {
            return required_features.contains(j.get<std::string>());
          });
        else if (choice.contains("components"))
          matches = std::count_if(choice["components"].begin(), choice["components"].end(), [&](const nlohmann::json &j) {
            return required_components.contains(j.get<std::string>());
          });
        else {
          spdlog::error("Invalid choice {}", c);
          return project::state::PROJECT_HAS_INVALID_COMPONENT;
        }
        if (matches == 0 && choice.contains("default")) {
          spdlog::info("Selecting default choice for {}", c);
          if (choice["default"].contains("feature")) {
            unprocessed_features.insert(choice["default"]["feature"].get<std::string>());
            unprocessed_choices.erase(c);
          } else if (choice["default"].contains("component")) {
            unprocessed_components.insert(choice["default"]["component"].get<std::string>());
            unprocessed_choices.erase(c);
          } else {
            spdlog::error("Invalid default choice in {}", c);
            return project::state::PROJECT_HAS_INVALID_COMPONENT;
          }
          break;
        }
      }
    }

    // Check if we have finished but we've come across replaced components
    if (unprocessed_components.empty() && unprocessed_features.empty() && unprocessed_replacements.size() != 0) {
      // move new replacements
      for (const auto &[replacement, id]: unprocessed_replacements) {
        spdlog::info("Adding {} to replaced_components", replacement);
        // replaced_components.insert(replacement);
        replacements.insert({ replacement, id });
      }
      unprocessed_replacements.clear();

      // Restart the whole process
      required_features.clear();
      required_components.clear();
      unprocessed_choices.clear();
      unprocessed_components.clear();
      unprocessed_features.clear();
      components.clear();
      project_summary["components"].clear();

      // Set the initial state
      for (const auto &c: initial_components)
        unprocessed_components.insert(c);
      for (const auto &f: initial_features)
        unprocessed_features.insert(f);

      spdlog::info("Start project processing again...");
    }

    // Check if we have finished but we have unprovided features
    if (unprocessed_components.empty() && unprocessed_features.empty() && unprovided_features.size() != 0) {
      std::unordered_set<std::string> temp_list = std::move(unprovided_features);

      // Look for any recommendations
      for (const auto &f: temp_list) {
        // Verify it hasn't been provided
        if (provided_features.contains(f))
          continue;
        if (feature_recommendations.contains(f)) {
          const auto &recommendation = feature_recommendations[f];
          if (recommendation.contains("component")) {
            const auto &component_name = recommendation["component"].get<std::string>();
            spdlog::info("Adding component '{}' for '{}'", component_name, f);
            unprocessed_components.insert(component_name);
          } else if (recommendation.contains("feature")) {
            const auto &feature_name = recommendation["feature"].get<std::string>();
            spdlog::info("Adding feature '{}' for '{}'", feature_name, f);
            unprocessed_features.insert(feature_name);
          }
        } else {
          unprovided_features.insert(f);
        }
      }
    }

    // Check if we have finished but our project is using SLCC files
    if (unprocessed_components.empty() && unprocessed_features.empty() && component_flags != component_database::flag::IGNORE_ALL_SLC) {
      // Find any features that aren't provided
      std::unordered_set<std::string> temp_require_list = std::move(slc_required);
      for (const auto &r: temp_require_list) {
        if (slc_provided.contains(r))
          continue;

        // Check the databases
        auto f = workspace.find_feature(r);
        if (!f.has_value()) {
          slc_required.insert(r);
          continue;
        }

        auto feature_node = f.value();
        std::unordered_set<std::string> recommended_options;
        std::unordered_set<std::string> other_options;

        // Go through possible options
        for (const auto &option: feature_node) {
          // Ignore if it is excluded
          if (option.is_object() && (!condition_is_fulfilled(option) || is_disqualified_by_unless(option)))
            continue;

          const auto name = option.is_object() ? option["name"].get<std::string>() : option.get<std::string>();

          // If this is recommended add to the recommended list, otherwise add to other options list
          if (slc_recommended.contains(name)) {
            if (condition_is_fulfilled(slc_recommended[name]) && !is_disqualified_by_unless(slc_recommended[name]))
              recommended_options.insert(name);
          } else {
            other_options.insert(name);
          }
        }

        // If there is more than 1 recommendation, the user must decide
        // If there is a single recommendation, use that
        // If there are no recommendations but only 1 option, use that
        if (recommended_options.size() > 1) {
          spdlog::error("Multiple recommendations for '{}'", r);
          slc_required.insert(r);
        } else if (recommended_options.size() == 1) {
          const auto name           = *recommended_options.begin();
          const auto recommend_node = slc_recommended[name];
          spdlog::info("Adding recommended component '{}' to satisfy '{}'", name, r);
          if (recommend_node.contains("instance")) {
            for (const auto &i: recommend_node["instance"]) {
              spdlog::info("Creating instance '{}' for '{}'", i.get<std::string>(), name);
              instances.insert({ name, i.get<std::string>() });
            }
          }
          // unprocessed_components.insert(name);
          add_component(name, component_database::flag::ONLY_SLCC);
        } else if (other_options.size() == 1) {
          const auto name = *other_options.begin();
          spdlog::info("Adding component '{}' to satisfy '{}'", name, r);
          add_component(name, component_database::flag::ONLY_SLCC);
          // unprocessed_components.insert(name);
        } else {
          slc_required.insert(r);
        }
      }
    }

    // Final check to see if Yakka component can provide an SLC requirement
    if (unprocessed_components.empty() && unprocessed_features.empty() && !slc_required.empty() && component_flags != component_database::flag::IGNORE_ALL_SLC) {
      std::unordered_set<std::string> temp_require_list = std::move(slc_required);
      for (const auto &r: temp_require_list) {
        if (slc_provided.contains(r))
          continue;

        // Try find a component with a matching name
        auto component_location = workspace.find_component(r, component_flags);
        if (component_location.has_value()) {
          auto [path, package] = component_location.value();
          spdlog::info("Adding component '{}' to satisfy '{}'", path.string(), r);
          unprocessed_components.insert(r);
        } else {
          slc_required.insert(r);
        }
      }
    }

    if (unprocessed_components.empty() && unprocessed_features.empty())
      break;
  }

  for (const auto &r: slc_required) {
    auto f = workspace.find_feature(r);
    if (f.has_value())
      spdlog::error("Found a possible provider for feature '{}' but there are multiple options:\n{}", r, f.value().dump(2));
    else
      spdlog::error("Failed to find provider for feature '{}'", r);
  }

  if (unknown_components.size() != 0)
    return project::state::PROJECT_HAS_UNKNOWN_COMPONENTS;

  if (slc_required.size() != 0 || unprovided_features.size() != 0)
    return project::state::PROJECT_HAS_UNRESOLVED_REQUIREMENTS;

  return project::state::PROJECT_VALID;
}

void project::evaluate_choices()
{
  // For each component, check each choice has exactly one match in required features
  for (const auto &c: components) {
    for (const auto &[choice_name, value]: c->json["choices"].items()) {
      int matches = 0;
      if (value.contains("features")) {
        matches = std::count_if(value["features"].begin(), value["features"].end(), [&](const auto &j) {
          return required_features.contains(j.template get<std::string>());
        });
      }
      if (value.contains("components")) {
        matches = std::count_if(value["components"].begin(), value["components"].end(), [&](const auto &j) {
          return required_components.contains(j.template get<std::string>());
        });
      }
      if (matches == 0) {
        incomplete_choices.push_back({ c->id, choice_name });
      } else if (matches > 1) {
        multiple_answer_choices.push_back(choice_name);
      }
    }
  }
}

void project::create_project_file()
{
  this->project_file = project_name + ".yakka";
  std::ofstream file(project_file);
  file << "name: " << project_name << "\n";
  file << "type: project\n";
  if (initial_components.size() != 0) {
    file << "components:\n";
    for (const auto &c: initial_components)
      file << "  - " << c << "\n";
  }
  if (initial_features.size() != 0) {
    file << "features:\n";
    for (const auto &f: initial_features)
      file << "  - " << f << "\n";
  }
  file << "data: ~\n";
  file.close();
}

void project::generate_project_summary()
{
  // Add standard information into the project summary
  project_summary["project_name"]   = project_name;
  project_summary["project_file"]   = project_file;
  project_summary["project_output"] = default_output_directory + project_name;
  project_summary["configuration"]  = workspace.summary["configuration"];

  if (!project_summary.contains("tools"))
    project_summary["tools"] = nlohmann::json::object();

  // Put all YAML nodes into the summary
  for (const auto &c: components) {
    project_summary["components"][c->id] = c->json;
    for (auto &[key, value]: c->json["tools"].items()) {
      inja::Environment inja_env = inja::Environment();
      inja_env.add_callback("curdir", 0, [&c](const inja::Arguments &args) {
        return std::filesystem::absolute(c->component_path).string();
      });

      project_summary["tools"][key] = try_render(inja_env, value.get<std::string>(), project_summary);
    }
  }

  project_summary["features"] = {};
  for (const auto &i: this->required_features)
    project_summary["features"].push_back(i);

  project_summary["initial"]               = {};
  project_summary["initial"]["components"] = {};
  project_summary["initial"]["features"]   = {};
  for (const auto &i: this->initial_components)
    project_summary["initial"]["components"].push_back(i);
  for (const auto &i: this->initial_features)
    project_summary["initial"]["features"].push_back(i);

  project_summary["data"]         = nlohmann::json::object();
  project_summary["host"]         = nlohmann::json::object();
  project_summary["host"]["name"] = host_os_string;
}

/**
 * @brief Parses the blueprints of the project.
 * 

 * For each component, it checks if it contains blueprints. If it does, it iterates over these blueprints.
 * For each blueprint, it renders a string using the inja_environment, based on whether the blueprint contains a regex or not.
 * It then logs this blueprint string, and adds a new blueprint to the blueprint_database, using the blueprint string as the key.
 * The new blueprint is created using the blueprint string, the blueprint value, and the directory of the component.
 */
void project::process_blueprints()
{
  for (const auto &c: components)
    process_blueprints(c);
}

void project::generate_target_database()
{
  std::vector<std::string> new_targets;
  std::unordered_set<std::string> processed_targets;
  std::vector<std::string> unprocessed_targets;

  for (const auto &c: commands)
    unprocessed_targets.push_back(c);

  while (!unprocessed_targets.empty()) {
    for (const auto &t: unprocessed_targets) {
      // Add to processed targets and check if it's already been processed
      if (processed_targets.insert(t).second == false)
        continue;

      // Do not add to task database if it's a data dependency. There is special processing of these.
      // if (t.front() == data_dependency_identifier)
      //   continue;

      // Check if target is not in the database. Note task_database is a multimap
      if (target_database.targets.find(t) == target_database.targets.end()) {
        const auto match = blueprint_database.find_match(t, this->project_summary);
        for (const auto &m: match) {
          // Add an entry to the database
          target_database.targets.insert({ t, m });

          // Check if the blueprint has additional requirements
          if (m->blueprint->requirements.size() != 0)
            for (const auto &t: m->blueprint->requirements) {
              if (additional_tools.contains(t))
                continue;
              const auto p = workspace.find_component(t);
              if (p.has_value()) {
                auto [component_path, db_path] = p.value();
                this->add_additional_tool(component_path);
              }
            }
        }
      }
      auto tasks = target_database.targets.equal_range(t);

      std::for_each(tasks.first, tasks.second, [&new_targets](auto &i) {
        if (i.second)
          new_targets.insert(new_targets.end(), i.second->dependencies.begin(), i.second->dependencies.end());
      });
    }

    unprocessed_targets.clear();
    unprocessed_targets.swap(new_targets);
  }
}

/**
 * @brief Save to disk the content of the @ref project_summary to project_summary_filename in the project output directory.
 *
 */
void project::save_summary()
{
  if (!fs::exists(project_summary["project_output"].get<std::string>()))
    fs::create_directories(project_summary["project_output"].get<std::string>());

  std::ofstream json_file(project_summary["project_output"].get<std::string>() + "/" + yakka::project_summary_filename);
  json_file << project_summary.dump(3);
  json_file.close();

  std::string template_contribution_filename = project_summary["project_output"].get<std::string>() + "/template_contributions.json";
  // Check if template contribution file exists
  if (fs::exists(template_contribution_filename)) {
    // Read the content and compare to the current value, only rewrite if content is different
    std::ifstream template_file_stream(template_contribution_filename);
    auto existing_template_contribution = nlohmann::json::parse(template_file_stream);
    auto patch                          = nlohmann::json::diff(template_contributions, existing_template_contribution);
    if (patch.size() == 0) {
      return;
    }
  }
  // Create the template contributions file
  std::ofstream template_contributions_file(template_contribution_filename);
  template_contributions_file << template_contributions.dump(3);
  template_contributions_file.close();
}

class custom_error_handler : public nlohmann::json_schema::basic_error_handler {
public:
  std::string component_name;
  void error(const nlohmann::json::json_pointer &ptr, const nlohmann::json &instance, const std::string &message) override
  {
    nlohmann::json_schema::basic_error_handler::error(ptr, instance, message);
    spdlog::error("Validation error in '{}': {} - {} : - {}", component_name, ptr.to_string(), instance.dump(3), message);
  }
};

void project::validate_schema()
{
  // Collect all the schema data
  nlohmann::json schema      = "{ \"properties\": {} }"_json;
  nlohmann::json data_schema = "{ \"properties\": {} }"_json;

  for (const auto &c: components) {
    if (c->json.contains("schema")) {
      const auto schema_json_pointer = "/properties"_json_pointer;
      json_node_merge(schema_json_pointer, schema["properties"], c->json["schema"]);
    }
    if (c->json.contains("data_schema")) {
      const auto schema_json_pointer = "/properties"_json_pointer;
      json_node_merge(schema_json_pointer, data_schema["properties"], c->json["data_schema"]);
    }
  }

  // Verify schema for each component
  {
    //spdlog::error("Schema: {}", schema.dump(2));
    // Create validator
    nlohmann::json_schema::json_validator validator(nullptr, nlohmann::json_schema::default_string_format_check);
    try {
      validator.set_root_schema(schema);
    } catch (const std::exception &e) {
      spdlog::error("Setting root schema for components failed\n{}", e.what());
      return;
    }

    // Iterate through each component and validate
    custom_error_handler err;
    for (const auto &c: components) {
      err.component_name = c->id;
      validator.validate(c->json, err);
    }
  }

  // Verify data schema for the project
  {
    // Create validator
    nlohmann::json_schema::json_validator validator(nullptr, nlohmann::json_schema::default_string_format_check);
    try {
      validator.set_root_schema(data_schema);
    } catch (const std::exception &e) {
      spdlog::error("Setting root schema for data failed\n{}", e.what());
      return;
    }

    custom_error_handler err;
    auto validation_result = validator.validate(project_summary["data"], err);
    if (!validation_result.is_null())
      current_state = state::PROJECT_HAS_FAILED_SCHEMA_CHECK;
  }
}

/**
 * @brief Updates the project data by merging all the component data into the project summary.
 */
void project::update_project_data()
{
  std::unordered_set<std::string> required_data;

  // Gather all the required data
  for (const auto &c: components)
    if (c->json.contains("/requires/data"_json_pointer))
      for (const auto &d: c->json["requires"]["data"]) {
        required_data.insert(d.get<std::string>());
      }

  // Merge all the component data into the project summary
  for (const auto &c: components) {
    for (const auto &r: required_data) {
      const auto pointer = nlohmann::json::json_pointer(r);
      if (!c->json.contains(pointer)) {
        continue;
      }

      if (!project_summary["data"].contains(pointer)) {
        project_summary["data"][pointer] = nlohmann::json::object();
      }

      json_node_merge(""_json_pointer, project_summary["data"][pointer], c->json[pointer]);
    }
  }

  // Apply schema with default values
}

bool project::is_disqualified_by_unless(const nlohmann::json &node)
{
  if (node.contains("unless"))
    for (const auto &u: node["unless"])
      if (required_features.contains(u.get<std::string>()))
        return true;

  return false;
}

bool project::condition_is_fulfilled(const nlohmann::json &node)
{
  if (node.contains("condition"))
    for (const auto &condition: node["condition"])
      if (!required_features.contains(condition.get<std::string>()))
        return false;

  return true;
}

void project::create_config_file(const std::shared_ptr<yakka::component> component, const nlohmann::json &config, const std::string &prefix, std::string instance_name)
{
  std::string config_filename = config["path"].get<std::string>();
  fs::path config_file_path   = component->component_path / config_filename;

  // Check for overrides
  if (config.contains("file_id")) {
    const auto file_id = config["file_id"].get<std::string>();
    if (slc_overrides.contains(file_id)) {
      auto overriding_components = slc_overrides.equal_range(file_id);
      for (auto c = overriding_components.first; c != overriding_components.second; ++c) {
        // Find the matching config, check conditions, and matching instance.
        for (const auto &i: c->second->json["config_file"]) {
          if (i.contains("override") && i["override"]["file_id"].get<std::string>() == file_id && !is_disqualified_by_unless(i) && condition_is_fulfilled(i)) {
            if (i["override"].contains("instance") && i["override"]["instance"].get<std::string>() == instance_name) {
              config_file_path = c->second->component_path / i["path"].get<std::string>();
              break;
            } else if (!i["override"].contains("instance")) {
              config_file_path = c->second->component_path / i["path"].get<std::string>();
              break;
            }
          }
        }
      }
    }
  }

  config_file_path          = this->inja_environment.render(config_file_path.generic_string(), { { "instance", prefix } });
  fs::path destination_path = fs::path{ default_output_directory + project_name + "/config" } / this->inja_environment.render(fs::path(config_filename).filename().string(), { { "instance", instance_name } });
  if (!instance_name.empty()) {
    // Convert instance name uppercase
    std::transform(instance_name.begin(), instance_name.end(), instance_name.begin(), ::toupper);
  }

  if (!fs::exists(config_file_path)) {
    spdlog::error("Failed to find config_file: {}", config_file_path.string());
    return;
  }

  // Create blueprints
  nlohmann::json blueprint = { { "depends", nullptr }, { "process", nullptr } };
  blueprint["depends"].push_back(config_file_path.string());
  blueprint["depends"].push_back("{{project_output}}/template_contributions.json");
  blueprint["process"].push_back({ { "inja", "{% set input = read_file(\"" + config_file_path.string() + "\" )%}{{replace(input, \"\\bINSTANCE\\b\", \"" + instance_name + "\")}}" } });
  blueprint["process"].push_back({ { "save", nullptr } });

  component->json["blueprints"][destination_path.string()] = blueprint;
  component->json["generated"]["includes"].push_back(destination_path.string());
}

void project::process_slc_rules()
{
  // Go through each SLC based component
  // std::vector<std::shared_ptr<yakka::component>>::size_type size = components.size();
  for (std::vector<std::shared_ptr<yakka::component>>::size_type i = 0; i < components.size(); ++i) {
    const auto &c = components[i];
    if (c->type == component::YAKKA_FILE)
      continue;

    auto instance_names               = instances.equal_range(c->id);
    const bool instantiable           = c->json.contains("instantiable");
    const std::string instance_prefix = (instantiable) ? c->json["instantiable"]["prefix"].get<std::string>() : "";

    // Process SLCE files and add every component found in the component paths
    if (c->type == component::SLCE_FILE) {
      std::unordered_set<std::filesystem::path> added_components;
      // Find all .slcc files in the component paths and add them
      for (const auto &p: c->json["component_path"]) {
        for (const auto &component_path: glob::rglob(p["path"].get<std::string>() + "/**/*.slcc")) {
          // Only add component if it hasn't been seen before
          if (added_components.insert(component_path).second == true) {
            std::shared_ptr<yakka::component> new_component = std::make_shared<yakka::component>();
            if (new_component->parse_file(component_path, "") == yakka::yakka_status::SUCCESS) {
              components.push_back(new_component);
              // ++size;
              // Process all the required components
              if (new_component->json.contains("requires") && new_component->json["requires"].contains("features"))
                for (const auto &r: new_component->json["requires"]["features"])
                  slc_required.insert(r.get<std::string>());
            }
          }
        }
      }
      evaluate_dependencies();
      continue;
    }

    // Process sources
    if (c->json.contains("source")) {
      for (const auto &p: c->json["source"]) {
        if (!p.contains("path"))
          continue;
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        fs::path source_path{ p["path"].get<std::string>() };
        if (source_path.extension() != ".h")
          c->json["sources"].push_back(p["path"]);
      }
    }

    // Process 'include'
    if (c->json.contains("include")) {
      for (const auto &p: c->json["include"]) {
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        c->json["includes"]["global"].push_back(p["path"]);
      }
    }

    // Process 'define'
    if (c->json.contains("define")) {
      for (const auto &p: c->json["define"]) {
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        nlohmann::json temp = p.contains("value") ? p : p["name"];
        if (instantiable) {
          if (temp.contains("value"))
            temp["name"] = this->inja_environment.render(temp["name"].get<std::string>(), { { "instance", instance_prefix } });
          else
            temp = this->inja_environment.render(temp.get<std::string>(), { { "instance", instance_prefix } });
        }
        c->json["defines"]["global"].push_back(temp);
      }
    }

    // Process library
    if (c->json.contains("library")) {
      for (const auto &p: c->json["library"]) {
        if (!p.contains("path"))
          continue;
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        fs::path source_path{ p["path"].get<std::string>() };
        c->json["libraries"].push_back(p["path"]);
      }
    }

    // Process template_contributions
    if (c->json.contains("template_contribution")) {
      for (const auto &t: c->json["template_contribution"]) {
        if (is_disqualified_by_unless(t) || !condition_is_fulfilled(t))
          continue;

        const auto name = t["name"].get<std::string>();
        if (instantiable && t.contains("value")) {
          if (t["value"].is_string()) {
            const auto value = t["value"].get<std::string>();
            for (auto i = instance_names.first; i != instance_names.second; ++i) {
              template_contributions[name].push_back(t);
              template_contributions[name].back()["value"] = this->inja_environment.render(value, { { "instance", i->second } });
            }
          } else if (t["value"].is_object()) {
            for (auto i = instance_names.first; i != instance_names.second; ++i) {
              template_contributions[name].push_back(t);
              for (auto &[key, value]: template_contributions[name].back()["value"].items()) {
                if (value.is_string())
                  template_contributions[name].back()["value"][key] = this->inja_environment.render(value.get<std::string>(), { { "instance", i->second } });
              }
            }
          } else {
            template_contributions[name].push_back(t);
          }
        } else {
          template_contributions[name].push_back(t);
        }
      }
    }

    // Process config_file
    if (c->json.contains("config_file")) {
      for (const auto &config: c->json["config_file"]) {
        if (!config.contains("path"))
          continue;
        if (is_disqualified_by_unless(config) || !condition_is_fulfilled(config))
          continue;
        if (instantiable && instance_names.first == instance_names.second)
          continue;
        if (config.contains("override"))
          continue;

        // Check if this component is instantiable and there are instances
        if (instantiable)
          for (auto i = instance_names.first; i != instance_names.second; ++i)
            create_config_file(c, config, instance_prefix, i->second);
        else
          create_config_file(c, config, instance_prefix, instance_prefix);
      }

      // Process 'template_file'
      if (c->json.contains("template_file")) {
        for (const auto &t: c->json["template_file"]) {
          if (is_disqualified_by_unless(t) || !condition_is_fulfilled(t))
            continue;

          fs::path template_file = t["path"].get<std::string>();
          fs::path target_file   = template_file.filename();
          target_file.replace_extension();

          const auto target = "{{project_output}}/generated/" + target_file.string();

          auto add_generated_item = [&](nlohmann::json &node) {
            // Create generated items
            if (target_file.extension() == ".c" || target_file.extension() == ".cpp")
              node["generated"]["sources"].push_back(target);
            else if (target_file.extension() == ".h" || target_file.extension() == ".hpp")
              node["generated"]["includes"].push_back(target);
            else if (target_file.extension() == ".ld")
              node["generated"]["linker_script"].push_back(target);
            else
              node["generated"]["files"].push_back(target);
          };

          add_generated_item(c->json);

          // Create blueprints
          nlohmann::json blueprint = { { "depends", nullptr }, { "process", nullptr } };
          blueprint["depends"].push_back({ { c->json["directory"].get<std::string>() + "/" + template_file.string() } });
          blueprint["depends"].push_back({ { "{{project_output}}/template_contributions.json" } });
          blueprint["process"].push_back({ { "jinja", "-t " + c->json["directory"].get<std::string>() + "/" + template_file.string() + " -d {{project_output}}/template_contributions.json" } });
          blueprint["process"].push_back({ { "save", nullptr } });

          c->json["blueprints"][target] = blueprint;
        }
      }

      // Process special toolchain settings
      if (c->json.contains("toolchain_settings")) {
        for (const auto &s: c->json["toolchain_settings"]) {
          if (s["option"] == "linkerfile") {
            if (is_disqualified_by_unless(s) || !condition_is_fulfilled(s))
              continue;

            c->json["generated"]["linker_script"] = "{{project_output}}/generated/" + fs::path{ s["value"].get<std::string>() }.filename().string();
          }
        }
      }
    }

    // Process toolchain settings
    project_summary["toolchain_settings"] = nlohmann::json::object();
    for (const auto &c: components) {
      if (c->json.contains("toolchain_settings") == false)
        continue;

      for (const auto &s: c->json["toolchain_settings"]) {
        if (is_disqualified_by_unless(s) || !condition_is_fulfilled(s))
          continue;

        const auto key = s["option"].get<std::string>();
        if (project_summary["toolchain_settings"].contains(key))
          if (project_summary["toolchain_settings"][key].is_array())
            project_summary["toolchain_settings"][key].push_back(s["value"]);
          else
            project_summary["toolchain_settings"][key] = nlohmann::json::array({ project_summary["toolchain_settings"][key], s["value"] });
        else
          project_summary["toolchain_settings"][key] = s["value"];
      }
    }
  }

  // Go through the template_contributions and sort via priorities
  nlohmann::json new_contributions;
  for (auto &item: template_contributions) {
    while (!item.is_null() && item.size() > 0) {
      // Remove the item with the lowest priority
      int lowest_priority       = INT_MAX;
      int lowest_priority_index = 0;
      for (size_t i = 0; i < item.size(); ++i) {
        int priority = item[i].contains("priority") ? item[i]["priority"].get<int>() : 0;
        if (priority < lowest_priority) {
          lowest_priority       = priority;
          lowest_priority_index = i;
        }
      }
      nlohmann::json entry = item[lowest_priority_index];
      spdlog::info("Ordering '{}' at priority {}", entry["name"].get<std::string>(), lowest_priority);

      //const std::string value = entry["value"].get<std::string>();
      new_contributions[entry["name"].get<std::string>()].push_back(entry["value"]);
      item.erase(lowest_priority_index);
    }
  }
  template_contributions = new_contributions;
}

void project::process_blueprints(const std::shared_ptr<component> c)
{
  if (c->json.contains("blueprints")) {
    for (const auto &[b_key, b_value]: c->json["blueprints"].items()) {
      std::string blueprint_string = try_render(inja_environment, b_value.contains("regex") ? b_value["regex"].get<std::string>() : b_key, project_summary);
      if (blueprint_string[0] == data_dependency_identifier && !blueprint_string.starts_with(":/data/")) {
        spdlog::error("Invalid data blueprint: {}", blueprint_string);
        continue;
      }
      spdlog::info("Additional blueprint: {}", blueprint_string);
      blueprint_database.blueprints.insert({ blueprint_string, std::make_shared<blueprint>(blueprint_string, b_value, c->json["directory"].get<std::string>()) });
    }
  }
}

void project::process_tools(const std::shared_ptr<component> c)
{
  if (c->json.contains("tools")) {
    for (auto &[key, value]: c->json["tools"].items()) {
      inja::Environment inja_env = inja::Environment();
      inja_env.add_callback("curdir", 0, [&c](const inja::Arguments &args) {
        return std::filesystem::absolute(c->component_path).string();
      });

      project_summary["tools"][key] = try_render(inja_env, value.get<std::string>(), project_summary);
    }
  }
}

void project::save_blueprints()
{
  blueprint_database.save(this->output_path / "blueprints.json");
}

void project::add_additional_tool(const fs::path component_path)
{
  // Load component
  auto tool_component = std::make_shared<component>();
  auto result         = tool_component->parse_file(component_path);
  if (result != yakka_status::SUCCESS)
    return;

  // Add blueprints and tools to project
  process_blueprints(tool_component);
  process_tools(tool_component);

  // Add component to project
  components.push_back(tool_component);
  additional_tools.insert(tool_component->id);
}

} /* namespace yakka */
