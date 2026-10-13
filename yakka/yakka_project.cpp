/**
 * @file yakka_project.cpp
 * @brief Implements project evaluation, dependency resolution, and output generation.
 */

#include "yakka.hpp"
#include "yakka_project.hpp"
#include "yakka_schema.hpp"
#include "utilities.hpp"

#include "spdlog/spdlog.h"
#include "glob/glob.h"
// #include <ryml/json-schema.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <fstream>
#include <chrono>
#include <thread>
#include <string>
#include <charconv>
#include <stdexcept>
#include <algorithm>

using namespace std;

namespace yakka {
using namespace std::chrono_literals;

namespace {
bool choice_values_equal(ryml::ConstNodeRef left, ryml::ConstNodeRef right)
{
  if (left.is_map() != right.is_map() || left.is_seq() != right.is_seq())
    return false;
  if (left.is_container()) {
    if (left.num_children() != right.num_children())
      return false;
    size_t index = 0;
    for (const auto child: left.children()) {
      if (left.is_map()) {
        if (!right.has_child(child.key()) || !choice_values_equal(child, right[child.key()]))
          return false;
      } else if (!choice_values_equal(child, right[index++])) {
        return false;
      }
    }
    return true;
  }
  ryml::Tree left_value, right_value;
  left_value.duplicate_contents(left.tree(), left.id(), left_value.root_id());
  right_value.duplicate_contents(right.tree(), right.id(), right_value.root_id());
  return ryml::emitrs_json<std::string>(left_value) == ryml::emitrs_json<std::string>(right_value);
}

bool data_choice_allows(ryml::ConstNodeRef choice, ryml::ConstNodeRef value)
{
  for (const auto option: choice["options"].children())
    if (choice_values_equal(option, value))
      return true;
  return false;
}

class condition_input_visitor : public inja::StatisticsVisitor {
  void visit(const inja::DataNode &node) override
  {
    input_variables.insert(node.name);
  }

public:
  std::set<std::string> input_variables;
};

std::string source_location(const component &component, ryml::ConstNodeRef node)
{
  std::string path;
  while (node.id() != component.root.id()) {
    const auto parent = node.parent();
    std::string segment;
    if (parent.is_seq()) {
      size_t index = 0;
      for (const auto child: parent.children()) {
        if (child.id() == node.id())
          break;
        ++index;
      }
      segment = std::to_string(index);
    } else {
      segment = ryml_string(node.key());
    }
    path = "/" + segment + path;
    node = parent;
  }
  return "component '" + ryml_string(component.id) + "' (" + component.file_path.generic_string() + ", " + path + ")";
}

/// @brief Adds a single 'tools' entry into the project summary, handling both the plain
/// 'name: command' form and the 'name: { command:, return_value_success: }' form.
void add_tool_entry(ryml::ConstNodeRef child, ryml::NodeRef project_summary, inja::Environment &inja_env, const std::filesystem::path &component_path)
{
  if (!child.has_key()) {
    spdlog::error("Found a tool that doesn't have a key: {}", ryml::emitrs_yaml<std::string>(child));
    return;
  }

  if (!project_summary.contains("tools"))
    project_summary["tools"] |= ryml::MAP;

  if (child.has_val()) {
    project_summary["tools"][child.key()] << try_render(inja_env, child.val(), project_summary);
    return;
  }

  if (child.is_map() && child.has_child("command") && child["command"].has_val()) {
    if (child.has_child("return_value_success")) {
      auto tool = project_summary["tools"][child.key()];
      tool |= ryml::MAP;
      tool["command"] << try_render(inja_env, child["command"].val<std::string>().value(), project_summary);
      const auto success_node = child["return_value_success"];
      auto success_seq        = tool["tool_return_success"];
      success_seq |= ryml::SEQ;
      if (success_node.is_seq()) {
        for (const auto &value_node: success_node.children())
          success_seq.append_child() << value_node.val<int>().value_or(0);
      } else if (success_node.has_val()) {
        success_seq.append_child() << success_node.val<int>().value_or(0);
      }
    } else {
      project_summary["tools"][child.key()] << try_render(inja_env, child["command"].val<std::string>().value(), project_summary);
    }
    return;
  }

  spdlog::error("Tool '{}' in component '{}' is missing a 'command' entry", ryml_string(child.key()), component_path.string());
}

/// @brief Records the source file timestamp and content hash so the next run can detect changes cheaply.
void record_component_file_state(ryml::NodeRef component_node, const std::filesystem::path &component_path)
{
  std::error_code error;
  const auto last_modified = std::filesystem::last_write_time(component_path, error);
  if (!error)
    component_node["yakka_file_timestamp"] << static_cast<int64_t>(last_modified.time_since_epoch().count());

  const auto hash = project_diff::hash_file(component_path);
  if (hash)
    component_node["yakka_file_hash"] << project_diff::to_hex(*hash);
}
} // namespace

project::project(yakka::workspace &workspace, const std::string project_name) : project_name(project_name), yakka_home_directory("/.yakka"), project_directory("."), workspace(workspace)
{
  // abort_build      = false;

  project_has_slcc = false;
  current_state    = yakka::project::state::PROJECT_VALID;
  component_flags  = component_database::flag::ALL_COMPONENTS;

  // Setup the project data
  project_data.reserve_arena(16 * 1024 * 1024);               // Pre-allocate 16MB for the project data to avoid ANY reallocations
  project_data.add_flags(ryml::Tree::TREEF_NO_ARENA_REALLOC); // Forbid arena reallocations to ensure pointer stability
  project_data.rootref() |= ryml::MAP;

  if (!project_name.empty()) {
    output_path          = yakka::default_output_directory + project_name;
    project_summary_file = output_path / yakka::project_summary_filename;
    project_file         = project_name + ".yakka";
  }

  project_data["initial_features"] |= ryml::SEQ;
  project_data["initial_components"] |= ryml::SEQ;
  project_data["commands"] |= ryml::SEQ;

  template_contributions = project_data.rootref().append_child() << ryml::key("template_contributions");
  template_contributions |= ryml::MAP;

  add_common_template_commands(inja_environment);
}

project::~project()
{
}

/// @brief Executes set_project_directory.

void project::set_project_directory(const std::string path)
{
  project_directory = path;
}

void project::parse_project_string(const std::vector<std::string> &project_string)
{
  init_project(project_string);
}

void project::process_build_string(const std::string word)
{
  if (word.empty())
    return;

  // Identify features, commands, and components

  if (word.front() == '+') {
    auto feature_node = project_data["initial_features"].append_child() << word.substr(1);
    this->initial_features.push_back(feature_node.val());
    unprocessed_features.insert(feature_node.val());
  } else if (word.back() == '!') {
    auto command_node = project_data["commands"].append_child() << word.substr(0, word.size() - 1);
    this->commands.insert(command_node.val());
  } else {
    auto component_node = project_data["initial_components"].append_child() << word;
    this->initial_components.push_back(component_node.val());
    unprocessed_components.insert(component_node.val());
  }
}

/// @brief Executes init_project.

void project::init_project(const std::vector<std::string> build_string_list)
{
  for (const auto &build_string: build_string_list)

    process_build_string(build_string);
  init_project();
}

/// @brief Executes init_project.

void project::init_project(const std::string build_string)
{
  std::stringstream ss(build_string);

  std::string word;
  while (std::getline(ss, word, ' '))
    process_build_string(word);
  init_project();
}

/// @brief Executes init_project.

void project::init_project(std::vector<ryml::csubstr> components, std::vector<ryml::csubstr> features, std::unordered_set<ryml::csubstr> commands)
{
  initial_features.clear();
  initial_components.clear();

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

/// @brief Executes init_project.

void project::init_project()
{
  if (project_name.empty()) {

    // Generate a project name based on the components and features
    for (auto c: initial_components)
      project_name += ryml_string(c) + "-";
    if (!initial_components.empty())
      project_name.pop_back();
    for (auto f: initial_features)
      project_name += "+" + ryml_string(f);

    output_path          = yakka::default_output_directory + project_name;
    project_summary_file = output_path / yakka::project_summary_filename;
    project_file         = project_name + ".yakka";
  }

  if (fs::exists(project_summary_file)) {
    project_summary_last_modified = fs::last_write_time(project_summary_file);
    auto file_content             = yakka::get_file_contents<std::string>(project_summary_file);
    if (file_content) {
      ryml::parse_in_arena(ryml::to_csubstr(*file_content), project_data.rootref());
      project_summary = project_data["current"];
    }
    // auto result                   = ryml_load_file(project_summary_file);
    // if (!result) {
    //   spdlog::error("Failed to load project summary file: {}", project_summary_file.generic_string());
    //   return;
    // }
    // project_summary = result.value().rootref();

    diff.capture_baseline(project_summary);
    detect_changed_components();
  } else {
    fs::create_directories(output_path);

    project_summary = project_data.rootref().append_child() << ryml::key("current");
    project_summary |= ryml::MAP;

    project_summary["choices"] |= ryml::MAP;
    project_summary["components"] |= ryml::MAP;
    project_summary["features"] |= ryml::SEQ;
    project_summary["tools"] |= ryml::MAP;
    project_summary["data"] |= ryml::MAP;
  }

  // Check if there is a project file
  if (fs::exists(project_file)) {
    auto node = ryml_load_file(project_file);
    if (!node) {
      spdlog::error("Failed to load project file: {}", project_file.generic_string());
    } else {
      // Merge data from the project file
      // json_node_merge(ryml::Pointer{ "/data" }, project_summary["data"], node->rootref(), &data_schema);

      // merge_nodes(project_summary["data"], node->rootref());
    }
  }
}

/// @brief Executes add_command.

void project::add_command(const std::string command)
{
  auto command_node = project_data["commands"].append_child() << command;

  commands.insert(command_node.val());
}

/// @brief Executes process_requirements.

void project::process_requirements(std::shared_ptr<yakka::component> component, ryml::ConstNodeRef child_node)
{
  record_requirement_sources(*component, child_node);
  // TODO: Implement ryml version - needs json_node_merge, ryml::Pointer, .contains(), .get<>(), .is_string(),

  // Merge the feature values into the parent component
  // json_node_merge(ryml::Pointer(""), component->root, child_node, &project_schema);
  if (component->root.has_child("features") && child_node.parent().id() == component->root["features"].id()) {
    for (const auto child: child_node.children())
      if (child.key() != "condition") {
        auto destination = component->root.has_child(child.key()) ? component->root[child.key()] : component->root.append_child();
        destination.set_key(child.key());
        merge_nodes(destination, child);
      }
  } else {
    merge_nodes(component->root, child_node);
  }

  // Process required components
  if (child_node.contains(_requires_components_pointer)) {
    // Add the item/s to the new_component list
    const auto node = child_node[_requires_components_pointer];
    if (node.has_val())
      unprocessed_components.insert(node.val());
    else if (node.is_seq())
      for (const auto &i: node)
        unprocessed_components.insert(i.val());
    else
      spdlog::error("Node '{}' has invalid 'requires'", child_node["requires"].val<std::string>().value());
  }

  // Process required features
  if (child_node.contains(_requires_features_pointer)) {
    const auto node = child_node[_requires_features_pointer];
    // Add the item/s to the new_features list
    if (node.has_val()) {
      const auto feature = node.has_val() ? node.val() : node["name"].val();
      if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
        slc_required.insert(feature);
      unprocessed_features.insert(feature);
      // If the feature has recommendations, add them to the feature_recommendations map
      if (node.is_map() && node.contains("recommends"))
        feature_recommendations.insert({ feature, node["recommends"] });
    } else if (node.is_seq())
      for (const auto &i: node) {
        const auto feature = i.has_val() ? i.val() : i["name"].val();
        if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
          slc_required.insert(feature);
        unprocessed_features.insert(feature);
        // If the feature has recommendations, add them to the feature_recommendations map
        if (i.is_map() && i.contains("recommends"))
          feature_recommendations.insert({ feature, i["recommends"] });
      }
    else
      spdlog::error("Node '{}' has invalid 'requires'", child_node["requires"].val<std::string>().value());
  }

  // Process provided features
  if (child_node.contains(_provides_features_pointer)) {
    auto child_node_provides = child_node["provides"]["features"];
    if (child_node_provides.has_val()) {
      const auto feature = child_node_provides.val();
      if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
        slc_provided.insert(feature);
      // unprocessed_features.insert(feature);
      provided_features.insert(feature);
    } else if (child_node_provides.is_seq())
      for (const auto &i: child_node_provides) {
        const auto feature = i.val();
        if (component->type == yakka::component::SLCC_FILE || component->type == yakka::component::SLCP_FILE)
          slc_provided.insert(feature);
        // unprocessed_features.insert(feature);
        provided_features.insert(feature);
      }
  }

  // Process choices
  if (child_node.contains("choices"))
    for (const auto choice: child_node["choices"].children()) {
      const auto choice_name = choice.key();
      if (!project_summary["choices"].has_child(choice_name)) {
        unprocessed_choices.insert(choice_name);
        choice_sources[ryml_string(choice_name)] = source_location(*component, choice);
        auto choices = project_summary["choices"];
        const auto after = choices.num_children() > 0 ? choices.last_child().id() : ryml::NONE;
        choices.tree()->duplicate(choice.tree(), choice.id(), choices.id(), after);
        project_summary["choices"][choice_name]["parent"] << component->id;
      }
    }

  // Process supported components
  if (child_node.contains(_supports_components_pointer)) {
    for (const auto &c: required_components)
      if (child_node["supports"]["components"].has_child(c)) {
        spdlog::info("Processing component '{}' in {}", c, component->root["name"].val<std::string>().value());
        process_requirements(component, child_node["supports"]["components"][c]);
      }
  }

  // Process supported features
  if (child_node.contains(_supports_features_pointer)) {
    for (const auto &f: required_features)
      if (child_node["supports"]["features"].has_child(f)) {
        spdlog::info("Processing feature '{}' in {}", f, component->root["name"].val<std::string>().value());
        process_requirements(component, child_node["supports"]["features"][f]);
      }
  }
}

/// @brief Compares every component recorded in the loaded summary against its source file, then clears
/// the sections of the summary that this run regenerates from scratch.

void project::detect_changed_components()
{
  diff.detect_component_changes(project_summary_last_modified);

  // A modified component file can add or drop feature requirements, so the previous feature list is
  // only safe to carry forward while every component file is untouched
  bool has_component_features = false;
  if (project_summary.has_child("components"))
    for (const auto &c: project_summary["components"])
      if (c.has_child("features") && c["features"].is_map())
        has_component_features = true;

  // Conditional features and their dependencies must be resolved from fresh data, not the cache.
  if (diff.components_modified.empty() && !has_component_features) {
    if (project_summary.has_child("features"))
      for (const auto &f: project_summary["features"]) {
        if (!f.has_val()) {
          spdlog::error("Invalid feature entry in project summary: '{}'", f.key());
          continue;
        }
        required_features.insert(f.val());
      }
  } else {
    spdlog::info("Recomputing the project feature list because components changed or declare features");
  }

  // Every component is re-parsed each run, so the regenerated sections must start empty to avoid
  // duplicate keys and stale entries accumulating in the summary
  for (const auto section: { "components", "features", "choices", "tools", "data", "initial", "configuration" }) {
    const auto key = ryml::to_csubstr(section);
    if (project_summary.has_child(key))
      project_summary.remove_child(key);
  }

  project_summary["components"] |= ryml::MAP;
  project_summary["features"] |= ryml::SEQ;
  project_summary["choices"] |= ryml::MAP;
  project_summary["tools"] |= ryml::MAP;
  project_summary["data"] |= ryml::MAP;
}

/// @brief Resolves the diff once the current summary has been fully regenerated.

void project::finalize_diff()
{
  diff.finalize(project_summary, required_components, required_features);
  spdlog::info("Project diff: {}", diff.describe());
}

/// @brief Executes add_component.

bool project::add_component(std::string &component_name, component_database::flag flags)
{
  return add_component(c4::to_csubstr(component_name), flags);
}

/// @brief Executes add_component.

bool project::add_component(c4::csubstr component_name, component_database::flag flags)
{
  // Convert string to id

  const auto component_id = component_dotname_to_id(component_name);

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
  auto new_component_node                         = project_summary["components"].append_child() << ryml::key(component_id);
  new_component_node |= ryml::MAP;
  if (new_component->parse_file(component_path, package_path, new_component_node) == yakka::yakka_status::SUCCESS) {
    components.push_back(new_component);
    record_component_file_state(new_component_node, component_path);
  } else {
    project_summary["components"].remove_child(component_id);
    current_state = project::state::PROJECT_HAS_INVALID_COMPONENT;
    return false;
  }

  // Add special processing of SLC related files and data
  if (new_component->type == yakka::component::YAKKA_FILE) {
    if (this->project_has_slcc)
      for (const auto &f: new_component->root["requires"]["slc"])
        slc_required.insert(f.val());
  } else if (new_component->type == yakka::component::SLCC_FILE) {
    project_has_slcc = true;
    unprocessed_components.insert("jinja");
    for (const auto &f: new_component->root["requires"]["features"])
      slc_required.insert(f.val());
    for (const auto &f: new_component->root["provides"]["features"])
      slc_provided.insert(f.val());
    for (const auto &r: new_component->root["recommends"]) {
      auto id        = r["id"].val();
      auto start_pos = id.find('%');
      auto end_pos   = id.rfind('%');
      if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos)
        id = id.sub(start_pos, end_pos - start_pos + 1);
      // id.erase(start_pos, end_pos - start_pos + 1);
      slc_recommended.insert({ id, r });
    }
    for (const auto instance: new_component->root["instances"].children())
      for (const auto i: instance.children())
        this->instances.insert({ instance.key(), i.val() });
    // Extract config overrides
    for (const auto &c: new_component->root["config_file"])
      if (c.contains("override")) {
        slc_overrides.insert({ c["override"]["file_id"].val(), new_component });
      }

  } else if (new_component->type == yakka::component::SLCP_FILE) {
    unprocessed_components.insert("jinja");
    for (const auto &f: new_component->root["requires"]["features"])
      slc_required.insert(f.val());
    for (const auto &r: new_component->root["recommends"]) {
      auto id        = r["id"].val();
      auto start_pos = id.find('%');
      auto end_pos   = id.rfind('%');
      if (start_pos != std::string::npos && end_pos != std::string::npos && start_pos < end_pos)
        id = id.sub(start_pos, end_pos - start_pos + 1);
      // id.erase(start_pos, end_pos - start_pos + 1);
      slc_recommended.insert({ id, r });
    }
    for (const auto instance: new_component->root["instances"].children())
      for (const auto i: instance.children())
        this->instances.insert({ instance.key(), i.val() });
  }

  // Add all the required components into the unprocessed list
  record_requirement_sources(*new_component, new_component->root);
  if (new_component->root.contains(_requires_components_pointer)) {
    for (const auto &r: new_component->root["requires"]["components"]) {
      unprocessed_components.insert(r.val());
      if (r.is_map() && r.has_child("instance")) {
        for (const auto &i: r["instance"])
          instances.insert({ r.val(), i.val() });
      }
    }
  }

  // Add all the required features into the unprocessed list
  if (new_component->root.contains(_requires_features_pointer))
    for (const auto &f: new_component->root["requires"]["features"]) {
      if (f.has_val())
        unprocessed_features.insert(f.val());
      else {
        unprocessed_features.insert(f["name"].val());
        if (f.has_child("recommends")) {
          feature_recommendations.insert({ f["name"].val(), f["recommends"] });
        }
      }
    }

  // Add all the provided features into the unprocessed list
  if (new_component->root.contains(_provides_features_pointer))
    for (const auto &f: new_component->root["provides"]["features"]) {
      // unprocessed_features.insert(f.val());
      provided_features.insert(f.val());
    }

  // Add all the component choices to the global choice list
  if (new_component->root.contains("choices"))
    for (auto choice: new_component->root["choices"].children()) {
      const auto choice_name = choice.key();
      if (!project_summary["choices"].contains(choice_name)) {
        unprocessed_choices.insert(choice_name);
        choice_sources[ryml_string(choice_name)] = source_location(*new_component, choice);
        choice.duplicate(project_summary["choices"], project_summary["choices"].last_child());
        // auto new_choice = project_summary["choices"].append_child();
        // new_choice |= ryml::MAP;
        // new_choice.set_key_serialized(choice_name);
        // new_choice.set_val_serialized(choice.val());
        project_summary["choices"][choice_name]["parent"] << new_component->id;
      }
    }

  if (new_component->root.contains(_replaces_components_pointer)) {
    const auto &replaced = new_component->root["replaces"]["components"].val();

    if (replacements.contains(replaced)) {
      if (replacements[replaced] != component_id) {
        spdlog::error("Multiple components replacing {}", replaced);
        current_state = project::state::PROJECT_HAS_MULTIPLE_REPLACEMENTS;
        return false;
      }
    } else {
      spdlog::info("{} replaces {}", ryml_string(component_id), replaced);
      unprocessed_replacements.insert({ replaced, component_id });
    }
  }

  // Process all the currently required features. Note new feature will be processed in the features pass
  if (new_component->root.contains(_supports_features_pointer)) {
    for (auto &f: required_features)
      if (new_component->root["supports"]["features"].has_child(f)) {
        spdlog::info("Processing required feature '{}' in {}", ryml_string(f), component_id);
        process_requirements(new_component, new_component->root["supports"]["features"][f]);
      }
  }
  if (new_component->root.contains(_supports_components_pointer)) {
    // Process the new components support for all the currently required components
    for (auto &c: required_components)
      if (new_component->root["supports"]["components"].has_child(c)) {
        spdlog::info("Processing required component '{}' in {}", ryml_string(c), component_id);
        process_requirements(new_component, new_component->root["supports"]["components"][c]);
      }
  }

  // Process schema data
  if (new_component->root.has_child("schema")) {
    project_schema.add_schema_data(new_component->root["schema"]);
  }
  if (new_component->root.has_child("data_schema")) {
    data_schema.add_schema_data(new_component->root["data_schema"]);
  }

  // Process all the existing components support for the new component
  for (auto &c: components)
    if (c->root.contains(_supports_components_pointer / component_id)) {
      spdlog::info("Processing component '{}' in {}", component_id, c->root["name"].val<std::string>().value());
      process_requirements(c, c->root["supports"]["components"][component_id]);
    }

  return true;
}

/// @brief Executes add_feature.

bool project::add_feature(c4::csubstr &feature_name)
{
  // Insert feature and continue if this is not new

  if (required_features.insert(feature_name).second == false)
    return false;

  if (!provided_features.contains(feature_name)) {
    unprovided_features.insert(feature_name);
  }

  // Process the feature "supports" for each existing component
  for (auto &c: components)
    if (c->root.contains(_supports_features_pointer / feature_name)) {
      spdlog::info("Processing feature '{}' in {}", feature_name, c->root["name"].val<std::string>().value());
      process_requirements(c, c->root["supports"]["features"][feature_name]);
    }

  return true;
}

/// @brief Executes process_choice.

project::state project::process_choice(c4::csubstr &choice_name)
{
  const auto &choice = project_summary["choices"][choice_name];

  if (choice.has_child("data")) {
    const auto path = choice["data"];
    if (!path.has_val() || path.val_is_null() || path.val().empty() || path.val().front() != '/' ||
        !choice.has_child("options") || !choice["options"].is_seq() || choice["options"].num_children() == 0) {
      spdlog::error("Invalid data choice '{}': expected a data path beginning with '/' and non-empty options in {}",
                    choice_name, choice_sources.at(ryml_string(choice_name)));
      return project::state::PROJECT_HAS_INVALID_COMPONENT;
    }
    if (choice.has_child("default")) {
      const auto value = choice["default"];
      bool valid = data_choice_allows(choice, value);
      if (!valid && value.is_seq() && choice.has_child("exclusive") && !choice["exclusive"].val<bool>().value()) {
        valid = value.num_children() > 0;
        for (const auto entry: value.children())
          valid = data_choice_allows(choice, entry) && valid;
      }
      if (!valid) {
        spdlog::error("Invalid default for data choice '{}' in {}", choice_name, choice_sources.at(ryml_string(choice_name)));
        return project::state::PROJECT_HAS_INVALID_COMPONENT;
      }
    }
    return project::state::PROJECT_VALID;
  }

  int matches      = 0;
  int option_count = 0;

  // Go through each option
  for (const auto &o: choice["options"]) {
    ++option_count;
    if (o.contains("feature")) {
      if (required_features.contains(o["feature"].val()))
        matches++;
    } else if (o.contains("component")) {
      if (required_components.contains(o["component"].val()))
        matches++;
    } else {
      spdlog::error("Invalid choice {}", choice_name);
      return project::state::PROJECT_HAS_INVALID_COMPONENT;
    }
  }

  // Check for an invalid choice where there is only one option but a default is specified
  if (option_count == 1 && choice.contains("default")) {
    spdlog::error("Invalid choice {}: has default choice for single option", choice_name);
    return project::state::PROJECT_HAS_INVALID_COMPONENT;
  }

  // Check we can use a default value
  if (matches == 0 && choice.contains("default") && option_count > 1) {
    spdlog::info("Selecting default choice for {}", choice_name);

    // Lambda to contain logic for adding default choice
    const auto add_default_choice = [&](ryml::ConstNodeRef choice_data) -> project::state {
      std::string source = "default of choice '" + ryml_string(choice_name) + "'";
      source += " at /default of " + choice_sources.at(ryml_string(choice_name));
      // TODO: Implement ryml version - needs .contains(), .get<>()
      if (choice_data.contains("feature")) {
        feature_sources[ryml_string(choice_data["feature"].val())].insert(source);
        unprocessed_features.insert(choice_data["feature"].val());
      } else if (choice_data.contains("component")) {
        component_sources[ryml_string(choice_data["component"].val())].insert(source);
        unprocessed_components.insert(choice_data["component"].val());
      } else {
        spdlog::error("Invalid choice {}: Default value is missing 'feature'/'component'", choice_name);
        return project::state::PROJECT_HAS_INVALID_COMPONENT;
      }
      return project::state::PROJECT_VALID;
    };

    if (choice["default"].is_seq()) {
      if (!choice.contains("type") || choice["type"].val() != "multi") {
        spdlog::error("Invalid choice '{}': Default has multiple values for non-multi choice", choice_name);
      }
      // Add all the default options
      for (const auto &default_option: choice["default"]) {
        const auto result = add_default_choice(default_option);
        if (result != project::state::PROJECT_VALID)
          return result;
      }
    } else {
      const auto result = add_default_choice(choice["default"]);
      if (result != project::state::PROJECT_VALID)
        return result;
    }
  }

  return project::state::PROJECT_VALID;
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
  while (true) {
    // Loop through the list of unprocessed components.
    // Note: Items will be added to unprocessed_components during processing
    component_list_t temp_component_list = std::move(unprocessed_components);
    for (auto i: temp_component_list) {
      // Try add the component
      if (!add_component(i, component_flags)) {
        if (current_state != yakka::project::state::PROJECT_VALID)
          return current_state;
      }
    }

    // Process all the new features
    // Note: Items will be added to unprocessed_features during processing
    feature_list_t temp_feature_list = std::move(unprocessed_features);
    for (auto f: temp_feature_list) {
      add_feature(f);
    }

    // Check if we have finished but we have unprocessed choices
    if (unprocessed_components.empty() && unprocessed_features.empty() && !unprocessed_choices.empty()) {
      // Move unprocessed choices to a temp list and clear unprocessed choices to allow unprocessed choices to be re-added as needed
      // auto temp_choices = std::move(unprocessed_choices);
      // unprocessed_choices.clear();
      for (auto c: unprocessed_choices) {
        auto result = process_choice(c);
        if (result != project::state::PROJECT_VALID)
          return result;
      }
    }

    // Check if we have finished but we've come across replaced components
    if (unprocessed_components.empty() && unprocessed_features.empty() && unprocessed_replacements.size() != 0) {
      // move new replacements
      for (auto &[replacement, id]: unprocessed_replacements) {
        spdlog::info("Adding {} to replaced_components", replacement);
        // replaced_components.insert(replacement);
        replacements.insert({ replacement, id });
      }
      unprocessed_replacements.clear();

      // Restart the whole process
      required_features.clear();
      provided_features.clear();
      unprovided_features.clear();
      activated_component_features.clear();
      feature_conditions.clear();
      feature_recommendations.clear();
      feature_sources.clear();
      component_sources.clear();
      choice_sources.clear();
      required_components.clear();
      unprocessed_choices.clear();
      unprocessed_components.clear();
      unprocessed_features.clear();
      components.clear();
      // clear_children() rather than clear(), which would also drop the node's key
      project_summary["components"].clear_children();
      project_summary["choices"].clear_children();

      // Set the initial state
      for (const auto &c: initial_components)
        unprocessed_components.insert(c);
      for (const auto &f: initial_features)
        unprocessed_features.insert(f);

      spdlog::info("Start project processing again...");
    }

    // Check if we have finished but we have unprovided features
    if (unprocessed_components.empty() && unprocessed_features.empty() && unprovided_features.size() != 0) {
      auto temp_list = std::move(unprovided_features);

      // Look for any recommendations
      for (const auto &f: temp_list) {
        // Verify it hasn't been provided
        if (provided_features.contains(f))
          continue;
        if (feature_recommendations.contains(f)) {
          const auto &recommendation = feature_recommendations[f];
          if (recommendation.contains("component")) {
            const auto &component_name = recommendation["component"].val();
            spdlog::info("Adding component '{}' for '{}'", component_name, f);
            unprocessed_components.insert(component_name);
          } else if (recommendation.contains("feature")) {
            const auto &feature_name = recommendation["feature"].val();
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
      auto temp_require_list = std::move(slc_required);
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
        std::unordered_set<ryml::csubstr> recommended_options;
        std::unordered_set<ryml::csubstr> other_options;

        // Go through possible options
        for (const auto &option: feature_node) {
          // Ignore if it is excluded
          if (option.is_map() && (!condition_is_fulfilled(option) || is_disqualified_by_unless(option)))
            continue;

          const auto name = option.is_map() ? option["name"].val() : option.val();

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
          auto name           = *recommended_options.begin();
          auto recommend_node = slc_recommended[name];
          spdlog::info("Adding recommended component '{}' to satisfy '{}'", name, r);
          if (recommend_node.contains("instance")) {
            for (const auto &i: recommend_node["instance"]) {
              spdlog::info("Creating instance '{}' for '{}'", i.val(), name);
              instances.insert({ name, i.val() });
            }
          }
          // unprocessed_components.insert(name);
          add_component(name, component_database::flag::ONLY_SLCC);
        } else if (other_options.size() == 1) {
          auto name = *other_options.begin();
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
      auto temp_require_list = std::move(slc_required);
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

    if (unprocessed_components.empty() && unprocessed_features.empty()) {
      const bool activated = unknown_components.empty() && evaluate_component_features();
      if (current_state != state::PROJECT_VALID)
        return current_state;
      if (activated)
        continue;
      break;
    }
  }

  for (const auto &r: slc_required) {
    auto f = workspace.find_feature(r);
    if (f.has_value()) {
      spdlog::error("Found a possible provider for feature '{}' but there are multiple options:\n{}", r, ryml::emitrs_yaml<std::string>(f.value()));
    } else
      spdlog::error("Failed to find provider for feature '{}'", r);
  }

  if (unknown_components.size() != 0)
    return project::state::PROJECT_HAS_UNKNOWN_COMPONENTS;

  if (slc_required.size() != 0 || unprovided_features.size() != 0)
    return project::state::PROJECT_HAS_UNRESOLVED_REQUIREMENTS;

  return project::state::PROJECT_VALID;
}

bool project::evaluate_component_features()
{
  bool has_features = false;
  for (const auto &c: components)
    if (c->type == component::YAKKA_FILE && c->root.has_child("features"))
      has_features = true;
  if (!has_features)
    return false;

  ryml::Tree evaluation_data;
  auto context = evaluation_data.rootref();
  context |= ryml::MAP;
  context["project_name"] << project_name;
  context["project_file"] << project_file.string();
  context["project_output"] << output_path.generic_string();
  context["host"] |= ryml::MAP;
  context["host"]["name"] << host_os_string;
  workspace.summary["configuration"].duplicate(context, context.last_child());
  project_summary["choices"].duplicate(context, context.last_child());
  context["components"] |= ryml::MAP;
  for (const auto &c: components)
    c->root.duplicate(context["components"], context["components"].last_child());
  context["features"] |= ryml::SEQ;
  for (const auto &f: required_features)
    context["features"].append_child() << f;

  std::vector<std::pair<c4::csubstr, c4::csubstr>> enabled;
  std::vector<std::string> errors;
  for (const auto &c: components) {
    if (c->type != component::YAKKA_FILE || !c->root.has_child("features"))
      continue;
    const auto declarations = c->root["features"];
    if (!declarations.is_map()) {
      errors.push_back("Component '" + ryml_string(c->id) + "': features must be a name-keyed map");
      continue;
    }
    for (const auto &feature: declarations) {
      const auto identity = std::make_pair(c->id, feature.key());
      if (activated_component_features.contains(identity))
        continue;
      try {
        if (!feature.is_map())
          throw std::runtime_error("feature declaration must be an object");
        const bool selected = required_features.contains(feature.key());
        if (!selected && !feature.has_child("condition"))
          continue;
        if (!selected && feature.has_child("condition")) {
          const auto condition = feature["condition"];
          if (!condition.has_val() || condition.val_is_null())
            throw std::runtime_error("condition must be a string expression");
          const auto key = std::make_pair(ryml_string(c->id), ryml_string(feature.key()));
          const auto expression = "{{" + ryml_string(condition.val()) + "}}";
          auto cached = feature_conditions.find(key);
          if (cached == feature_conditions.end() || cached->second.parsed_template->content != expression) {
            // Keep the source at a stable address: AST literals hold views into it.
            auto parsed = std::unique_ptr<inja::Template>(new inja::Template(inja_environment.parse(expression)));
            condition_input_visitor visitor;
            parsed->root.accept(visitor);
            cached = feature_conditions.insert_or_assign(key, feature_condition { std::move(parsed), std::move(visitor.input_variables) }).first;
          }
          collect_required_data(context["data"], cached->second.input_variables);
          const auto result = inja_environment.render(*cached->second.parsed_template, context);
          if (result == "false")
            continue;
          if (result != "true")
            throw std::runtime_error("condition must evaluate to true or false, got '" + result + "'");
        }
        enabled.push_back(identity);
      } catch (const std::exception &e) {
        errors.push_back("Component '" + ryml_string(c->id) + "', feature '" + ryml_string(feature.key()) + "': " + e.what());
      }
    }
  }

  // A newly enabled feature may supply data needed by another condition in the next pass.
  if (enabled.empty() && !errors.empty()) {
    for (const auto &error: errors)
      spdlog::error("Feature condition error: {}", error);
    current_state = state::PROJECT_HAS_INVALID_COMPONENT;
    return false;
  }
  for (auto &[component_id, feature]: enabled) {
    const bool selected = required_features.contains(feature);
    if (!selected)
      for (const auto &c: components)
        if (c->id == component_id)
          feature_sources[ryml_string(feature)].insert(source_location(*c, c->root["features"][feature]));
    activated_component_features.emplace(component_id, feature);
    provided_features.insert(feature);
    unprovided_features.erase(feature);
    unprocessed_features.insert(feature);
    for (const auto &c: components)
      if (c->id == component_id)
        process_requirements(c, c->root["features"][feature]);
  }
  return !enabled.empty();
}

/// @brief Executes evaluate_choices.

void project::evaluate_choices()
{
  incomplete_choices.clear();
  multiple_answer_choices.clear();
  invalid_data_choices.clear();
  update_project_data();
  for (const auto choice: project_summary["choices"].children()) {
    if (choice.has_child("data")) {
      const auto pointer = ryml::Pointer(choice["data"].val());
      const auto data = project_summary["data"];
      if (!data.contains(pointer)) {
        incomplete_choices.emplace_back(choice["parent"].val(), choice.key());
        continue;
      }
      const auto value = data.at(pointer);
      if (data_choice_allows(choice, value))
        continue;
      if (value.is_seq() && value.num_children() > 0) {
        bool valid = true;
        for (const auto entry: value.children())
          valid = data_choice_allows(choice, entry) && valid;
        if (valid) {
          if (value.num_children() > 1 && (!choice.has_child("exclusive") || choice["exclusive"].val<bool>().value()))
            multiple_answer_choices.push_back(choice.key());
          continue;
        }
      }
      invalid_data_choices[ryml_string(choice.key())] = ryml::emitrs_json<std::string>(value);
      current_state = state::PROJECT_HAS_INVALID_COMPONENT;
      continue;
    }
    size_t matches = 0;
    for (const auto option: choice["options"].children()) {
      if (option.has_child("feature") && required_features.contains(option["feature"].val()))
        ++matches;
      else if (option.has_child("component") && required_components.contains(option["component"].val()))
        ++matches;
    }
    if (matches == 0 && choice["options"].num_children() > 1)
      incomplete_choices.emplace_back(choice["parent"].val(), choice.key());
    else if (matches > 1 && (!choice.has_child("exclusive") || choice["exclusive"].val<bool>().value()))
      multiple_answer_choices.push_back(choice.key());
  }
}

void project::record_requirement_sources(const component &component, ryml::ConstNodeRef node)
{
  for (const auto kind: { "features", "components" }) {
    if (!node.has_child("requires") || !node["requires"].has_child(kind))
      continue;
    auto &sources = std::string_view(kind) == "features" ? feature_sources : component_sources;
    const auto requirements = node["requires"][kind];
    const auto record = [&](ryml::ConstNodeRef requirement) {
      const auto name = requirement.has_val() ? requirement.val() : requirement["name"].val();
      sources[ryml_string(name)].insert(source_location(component, requirement));
    };
    if (requirements.is_seq())
      for (const auto requirement: requirements.children())
        record(requirement);
    else
      record(requirements);
  }
}

std::string project::evaluation_error() const
{
  std::string message = "Project evaluation failed";
  const auto append_sources = [&](c4::csubstr name, bool feature) {
    const auto &sources = feature ? feature_sources : component_sources;
    if (const auto found = sources.find(ryml_string(name)); found != sources.end())
      for (const auto &source: found->second)
        message += "\n    selected by " + source;
    const auto &initial = feature ? initial_features : initial_components;
    if (std::find(initial.begin(), initial.end(), name) != initial.end())
      message += "\n    selected on the command line";
  };
  std::set<std::string> unresolved;
  for (const auto feature: unprovided_features)
    if (!provided_features.contains(feature))
      unresolved.insert(ryml_string(feature));
  for (const auto feature: slc_required)
    if (!slc_provided.contains(feature))
      unresolved.insert(ryml_string(feature));
  for (const auto &feature: unresolved) {
    message += "\n  Unresolved feature '" + feature + "': no provider selected.";
    append_sources(ryml::to_csubstr(feature), true);
    message += "\n    Add a component that provides this feature via provides.features (or an enabled features declaration),"
               " or correct/remove the requirement. Feature names are case-sensitive; supports.features alone is not a provider.";
  }
  const auto append_choice = [&](c4::csubstr name, bool conflict) {
    const auto choice = project_summary["choices"][name];
    message += "\n  Choice '" + ryml_string(name) + "'";
    if (choice.has_child("description"))
      message += " (" + ryml_string(choice["description"].val()) + ")";
    message += " in " + choice_sources.at(ryml_string(name));
    message += conflict ? " is exclusive but has multiple selections:" : " has no selection. Available options:";
    if (choice.has_child("data")) {
      message += "\n    data path '" + ryml_string(choice["data"].val()) + "'";
      for (const auto option: choice["options"].children())
        message += "\n    " + ryml::emitrs_json<std::string>(option);
      message += conflict ? "\n    Keep exactly one option, or use exclusive: false to allow an array of selections."
                          : "\n    Set the data path in component data or --data, or specify a choice default.";
      return;
    }
    for (const auto option: choice["options"].children()) {
      const bool feature = option.has_child("feature");
      const auto selected = option[feature ? "feature" : "component"].val();
      if (conflict && !(feature ? required_features.contains(selected) : required_components.contains(selected)))
        continue;
      message += "\n    " + std::string(feature ? "feature '" : "component '") + ryml_string(selected) + "'";
      if (conflict)
        append_sources(selected, feature);
    }
    message += conflict
      ? "\n    Keep exactly one option: correct the requirements, enabled feature declarations, or defaults listed above."
        " Use exclusive: false only if these options genuinely can coexist."
      : "\n    Select one option on the command line (use +feature for a feature), in requires, or with a choice default.";
  };
  for (const auto &[component, choice]: incomplete_choices)
    append_choice(choice, false);
  for (const auto choice: multiple_answer_choices)
    append_choice(choice, true);
  for (const auto &[name, value]: invalid_data_choices) {
    const auto choice = project_summary["choices"][ryml::to_csubstr(name)];
    message += "\n  Data choice '" + name + "' in " + choice_sources.at(name) +
               " has an invalid value at '" + ryml_string(choice["data"].val()) + "': " + value +
               "\n    Select only values listed in options.";
  }
  if (unresolved.empty() && incomplete_choices.empty() && multiple_answer_choices.empty() && invalid_data_choices.empty()) {
    switch (current_state) {
      case state::PROJECT_HAS_UNKNOWN_COMPONENTS: message += ": unknown components"; break;
      case state::PROJECT_HAS_REMOTE_COMPONENTS: message += ": remote components need to be fetched"; break;
      case state::PROJECT_HAS_INVALID_COMPONENT: message += ": invalid component"; break;
      case state::PROJECT_HAS_MULTIPLE_REPLACEMENTS: message += ": multiple components replace the same component"; break;
      case state::PROJECT_HAS_FAILED_SCHEMA_CHECK: message += ": schema validation failed"; break;
      default: message += ": dependency or choice evaluation failed"; break;
    }
    message += ". See the preceding diagnostics and yakka.log for details.";
  }
  return message;
}

/// @brief Executes create_project_file.

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

/// @brief Executes generate_project_summary.

void project::generate_project_summary()
{
  // Add standard information into the project summary

  project_summary["project_name"] << project_name;
  project_summary["project_file"] << project_file.string();
  project_summary["project_output"] << default_output_directory + project_name;
  // project_summary["configuration"]  << workspace.summary["configuration"];
  if (project_summary.has_child("configuration"))
    project_summary.remove_child("configuration");
  workspace.summary["configuration"].duplicate(project_summary, project_summary.last_child());

  // TODO: Implement ryml version - needs json::object()
  if (!project_summary.contains("tools"))
    project_summary["tools"] |= ryml::MAP;

  // Put all YAML nodes into the summary
  for (const auto &c: components) {
    // c->root.move(project_summary["components"], project_summary["components"].last_child());
    // c->root.duplicate(project_summary["components"], project_summary["components"].last_child());
    // project_summary["components"][c->id] << c->root;
    if (c->root.contains("tools")) {
      for (auto child: c->root["tools"].children()) {
        inja::Environment inja_env;
        /// @brief Executes add_callback.

        inja_env.add_callback("curdir", 0, [&c](inja::Arguments &args, ryml::NodeRef additional_data) {
          return additional_data["values"].append_child() << std::filesystem::absolute(c->component_path).string();
        });

        add_tool_entry(child, project_summary, inja_env, c->component_path);
      }
    }
  }

  project_summary["features"].clear_children();
  for (auto &i: this->required_features)
    project_summary["features"].append_child() << i;

  project_summary["initial"] |= ryml::MAP;
  project_summary["initial"]["components"] |= ryml::SEQ;
  project_summary["initial"]["features"] |= ryml::SEQ;

  for (auto &i: this->initial_components)
    project_summary["initial"]["components"].append_child() << i;
  for (auto &i: this->initial_features)
    project_summary["initial"]["features"].append_child() << i;

  // TODO: Implement ryml version - needs json::object()
  project_summary["data"] |= ryml::MAP;
  project_summary["host"] |= ryml::MAP;
  project_summary["host"]["name"] << host_os_string;
}

/**
 * @brief Parses the blueprints of the project.
 * 

 * For each component, it checks if it contains blueprints. If it does, it iterates over these blueprints.
 * For each blueprint, it renders a string using the inja_environment, based on whether the blueprint contains a regex or not.
 * It then logs this blueprint string, and adds a new blueprint to the blueprint_database, using the blueprint string as the key.

/// @brief Executes process_blueprints.

 * The new blueprint is created using the blueprint string, the blueprint value, and the directory of the component.
 */
void project::process_blueprints()
{
  for (const auto &c: components)
    process_blueprints(c);
}

/// @brief Executes generate_target_database.

void project::generate_target_database()
{
  std::vector<ryml::csubstr> new_targets;

  std::unordered_set<ryml::csubstr> processed_targets;
  std::vector<ryml::csubstr> unprocessed_targets;

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
      auto matches = target_database.add_target(t, blueprint_database, project_summary);

      for (const auto &m: matches) {
        // Check if the blueprint has additional requirements
        for (const auto &t: m->blueprint->requirements) {
          if (additional_tools.contains(t))
            continue;
          const auto p = workspace.find_component(t);
          if (p.has_value()) {
            auto [component_path, db_path] = p.value();
            this->add_additional_tool(component_path);
          }
        }

        // Add any new targets to the unprocessed list
        new_targets.insert(new_targets.end(), m->dependencies.begin(), m->dependencies.end());
      }
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
  if (!fs::exists(project_summary["project_output"].val<std::string>().value()))
    fs::create_directories(project_summary["project_output"].val<std::string>().value());

  const fs::path project_summary_path = ryml_path(project_summary["project_output"].val()) / yakka::project_summary_filename;
  ryml_save_file(project_summary_path, project_summary);

  const fs::path template_contribution_filename = ryml_path(project_summary["project_output"].val()) / "template_contributions.json";

  // Check if template contribution file exists
  if (fs::exists(template_contribution_filename)) {
    // Read the content and compare to the current value, only rewrite if content is different
    auto existing_template_contribution = ryml_load_file(template_contribution_filename);
    if (!existing_template_contribution.has_value()) {
      spdlog::error("Failed to parse existing template contribution file '{}'", template_contribution_filename.generic_string());
    } else {
      // TODO: Implement ryml diffing and patching to preserve comments and formatting - needs ryml::Tree::diff() and patch application
      // auto patch = ryml::Tree::diff(template_contributions, *existing_template_contribution);
      // if (patch.size() == 0) {
      //   return;
      // }
      if (!template_contributions.empty())
        ryml_save_file(template_contribution_filename, template_contributions);
    }
  } else {
    // Create the template contributions file
    if (!template_contributions.empty())
      ryml_save_file(template_contribution_filename, template_contributions);
  }
}

/// @brief Executes validate_schema.

void project::validate_schema()
{
  // Verify schema for each component

  for (const auto &c: components) {
    if (project_schema.validate(c->root, c->id, c->file_path) == false)
      current_state = state::PROJECT_HAS_FAILED_SCHEMA_CHECK;
  }

  // Verify data schema for the project
  if (data_schema.validate(project_summary["data"], "project_data") == false)
    current_state = state::PROJECT_HAS_FAILED_SCHEMA_CHECK;
}

/**
 * @brief Updates the project data by merging all the component data into the project summary.
 */
void project::update_project_data()
{
  collect_required_data(project_summary["data"]);
}

void project::collect_required_data(ryml::NodeRef data, const std::set<std::string> &input_variables)
{
  data |= ryml::MAP;
  data.clear_children();
  std::set<std::string> required_data;
  for (const auto &variable: input_variables) {
    const auto path = inja::DataNode::convert_dot_to_ptr(variable);
    if (path.starts_with("/data/"))
      required_data.insert(path.substr(5));
  }

  // Gather all the required data
  for (const auto &c: components)
    if (c->root.contains(ryml::Pointer("/requires/data")))
      for (const auto &d: c->root.at(ryml::Pointer("/requires/data"))) {
        required_data.insert(ryml::Pointer(d.val()).to_string());
      }
  for (const auto choice: project_summary["choices"].children())
    if (choice.has_child("data"))
      required_data.insert(ryml_string(choice["data"].val()));

  // Merge all the component data into the requested destination.
  for (const auto &c: components) {
    for (const auto &r: required_data) {
      bool covered = !r.empty() && required_data.contains("");
      for (auto separator = r.find('/', 1); separator != std::string::npos; separator = r.find('/', separator + 1))
        if (required_data.contains(r.substr(0, separator))) {
          covered = true;
          break;
        }
      if (covered)
        continue;
      const auto pointer = ryml::Pointer(ryml::to_csubstr(r));
      if (!c->root.contains(pointer)) {
        continue;
      }
      auto component_node = c->root.at(pointer);

      if (!data.contains(pointer)) {
        if (component_node.is_seq())
          data[pointer] |= ryml::SEQ;
        else if (component_node.is_map())
          data[pointer] |= ryml::MAP;
        else
          data[pointer] |= ryml::VAL;
        // Pointer seeds borrow their final key; the requested paths are temporary.
        if (!pointer.empty())
          data.at(pointer).set_key_serialized(pointer[pointer.size() - 1]);
        json_node_merge(pointer, data.at(pointer), component_node);
      } else {
        json_node_merge(pointer, data.at(pointer), component_node, &data_schema);
      }

      auto test_node = data.at(pointer);
      if (!test_node.is_seq() && !test_node.is_map() && !test_node.has_val()) {
        spdlog::error("BAD");
      }
    }
  }

  if (project_data.rootref().has_child("request_data"))
    merge_nodes(data, project_data["request_data"]);

  for (const auto choice: project_summary["choices"].children()) {
    if (!choice.has_child("data"))
      continue;
    const auto pointer = ryml::Pointer(choice["data"].val());
    if (project_data.rootref().has_child("request_data") && project_data["request_data"].contains(pointer)) {
      const auto requested = project_data["request_data"].at(pointer);
      if (!data.contains(pointer))
        data[pointer] |= ryml::VAL;
      auto destination = data[pointer];
      destination.clear_children();
      destination.tree()->duplicate_contents(requested.tree(), requested.id(), destination.id());
      if (!pointer.empty())
        destination.set_key_serialized(pointer[pointer.size() - 1]);
      continue;
    }
    if (!choice.has_child("default"))
      continue;
    if (data.contains(pointer))
      continue;
    auto destination = data[pointer];
    destination |= ryml::VAL;
    destination.tree()->duplicate_contents(choice.tree(), choice["default"].id(), destination.id());
    if (!pointer.empty())
      destination.set_key_serialized(pointer[pointer.size() - 1]);
  }

  // Apply schema with default values
}

/// @brief Executes is_disqualified_by_unless.

bool project::is_disqualified_by_unless(ryml::ConstNodeRef node)
{
  if (node.contains("unless"))

    for (const auto &u: node["unless"])
      if (required_features.contains(u.val()))
        return true;

  return false;
}

/// @brief Executes condition_is_fulfilled.

bool project::condition_is_fulfilled(ryml::ConstNodeRef node)
{
  if (node.contains("condition"))

    for (const auto &condition: node["condition"])
      if (!required_features.contains(condition.val()))
        return false;

  return true;
}

/// @brief Executes create_config_file.

void project::create_config_file(const std::shared_ptr<yakka::component> component, ryml::ConstNodeRef config, const std::string &prefix, std::string instance_name)
{
  std::string config_filename = config["path"].val<std::string>().value();

  std::filesystem::path config_file_path = component->component_path / config_filename;

  // Check for overrides
  if (config.contains("file_id")) {
    const auto file_id = config["file_id"].val();
    if (slc_overrides.contains(file_id)) {
      auto overriding_components = slc_overrides.equal_range(file_id);
      for (auto c = overriding_components.first; c != overriding_components.second; ++c) {
        // Find the matching config, check conditions, and matching instance.
        for (const auto &i: c->second->root["config_file"]) {
          if (i.contains("override") && i["override"]["file_id"].val() == file_id && !is_disqualified_by_unless(i) && condition_is_fulfilled(i)) {
            if (i["override"].contains("instance") && i["override"]["instance"].val() == instance_name) {
              config_file_path = c->second->component_path / i["path"].val<std::string>().value();
              break;
            } else if (!i["override"].contains("instance")) {
              config_file_path = c->second->component_path / i["path"].val<std::string>().value();
              break;
            }
          }
        }
      }
    }
  }

  ryml::Tree temp_tree;
  temp_tree["instance"] << prefix;
  config_file_path = this->inja_environment.render(config_file_path.generic_string(), temp_tree.rootref());

  temp_tree["instance"] << instance_name;
  std::filesystem::path destination_path = std::filesystem::path{ default_output_directory + project_name + "/config" } / this->inja_environment.render(std::filesystem::path(config_filename).filename().string(), temp_tree.rootref());
  if (!instance_name.empty()) {
    // Convert instance name uppercase
    std::transform(instance_name.begin(), instance_name.end(), instance_name.begin(), ::toupper);
  }

  if (!fs::exists(config_file_path)) {
    spdlog::error("Failed to find config_file: {}", config_file_path.string());
    return;
  }

  // Create blueprints
  ryml::NodeRef blueprint = component->blueprints.append_child();
  blueprint.set_key_serialized(destination_path.string());
  blueprint |= ryml::MAP;
  blueprint["depends"] |= ryml::SEQ;
  blueprint["process"] |= ryml::SEQ;
  blueprint["depends"].append_child() << config_file_path.string();
  blueprint["depends"].append_child() << "{{project_output}}/template_contributions.json";
  auto process_node = blueprint["process"].append_child();
  process_node |= ryml::MAP;
  process_node["inja"] << "{% set input = read_file(\"" + config_file_path.string() + "\" )%}{{replace(input, \"\\bINSTANCE\\b\", \"" + instance_name + "\")}}";
  process_node = blueprint["process"].append_child();
  process_node |= ryml::MAP;
  process_node["save"] << nullptr;

  component->root["generated"]["includes"].append_child() << destination_path.string();
}

/// @brief Executes process_slc_rules.

void project::process_slc_rules()
{
  // Go through each SLC based component

  // std::vector<std::shared_ptr<yakka::component>>::size_type size = components.size();
  for (std::vector<std::shared_ptr<yakka::component>>::size_type i = 0; i < components.size(); ++i) {
    const auto &c = components[i];
    if (c->type == component::YAKKA_FILE)
      continue;

    auto instance_names               = instances.equal_range(c->id);
    const bool instantiable           = c->root.contains("instantiable");
    const std::string instance_prefix = (instantiable) ? c->root["instantiable"]["prefix"].val<std::string>().value() : "";

    // Process SLCE files and add every component found in the component paths
    if (c->type == component::SLCE_FILE) {
      std::unordered_set<std::filesystem::path> added_components;
      // Find all .slcc files in the component paths and add them
      for (const auto &p: c->root["component_path"]) {
        for (const auto &component_path: glob::rglob(p["path"].val<std::string>().value() + "/**/*.slcc")) {
          // Only add component if it hasn't been seen before
          if (added_components.insert(component_path).second == true) {
            std::shared_ptr<yakka::component> new_component = std::make_shared<yakka::component>();
            auto new_component_node                         = project_summary["components"].append_child() << ryml::Key("__temp__");
            new_component_node |= ryml::MAP;
            if (new_component->parse_file(component_path, "", new_component_node) == yakka::yakka_status::SUCCESS) {
              components.push_back(new_component);
              // Set the key to the ID
              new_component_node << ryml::key(new_component->id);

              // ++size;
              // Process all the required components
              if (new_component->root.contains("requires") && new_component->root["requires"].contains("features"))
                for (const auto &r: new_component->root["requires"]["features"])
                  slc_required.insert(r.val());
            }
          }
        }
      }
      evaluate_dependencies();
      continue;
    }

    // Process sources
    if (c->root.contains("source")) {
      for (const auto &p: c->root["source"]) {
        if (!p.contains("path"))
          continue;
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        std::filesystem::path source_path{ p["path"].val<std::string>().value() };
        if (source_path.extension() != ".h")
          c->root["sources"].append_child() << p["path"].val();
      }
    }

    // Process 'include'
    if (c->root.contains("include")) {
      for (const auto &p: c->root["include"]) {
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        c->root["includes"]["global"].append_child() << p["path"].val();
      }
    }

    // Process 'define'
    if (c->root.contains("define")) {
      for (auto p: c->root["define"]) {
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        // TODO: Implement ryml version - needs ternary operator with ryml nodes
        auto temp = p.contains("value") ? p : p["name"];
        if (instantiable) {
          ryml::Tree temp_tree;
          temp_tree["instance"] << instance_prefix;
          if (temp.has_child("value"))
            temp["name"] << this->inja_environment.render(temp["name"].val<std::string>().value(), temp_tree.rootref());
          else
            temp << this->inja_environment.render(temp.val<std::string>().value(), temp_tree.rootref());
        }
        temp.duplicate(c->root["defines"]["global"].last_child());
        // c->root["defines"]["global"].append_child() << temp;
      }
    }

    // Process library
    if (c->root.contains("library")) {
      for (auto p: c->root["library"]) {
        if (!p.contains("path"))
          continue;
        if (is_disqualified_by_unless(p) || !condition_is_fulfilled(p))
          continue;

        std::filesystem::path source_path{ p["path"].val<std::string>().value() };
        c->root["libraries"].append_child() << p["path"].val();
      }
    }

    // Process template_contributions
    if (c->root.contains("template_contribution")) {
      for (auto t: c->root["template_contribution"]) {
        if (is_disqualified_by_unless(t) || !condition_is_fulfilled(t))
          continue;

        const auto name = t["name"].val();
        if (instantiable && t.contains("value")) {
          if (t["value"].has_val()) {
            // const auto value = t["value"].val();
            for (auto i = instance_names.first; i != instance_names.second; ++i) {
              ryml::Tree temp_tree;
              temp_tree["instance"] << i->second;
              auto new_node = t.duplicate(template_contributions[name], template_contributions[name].last_child());
              // auto new_node = template_contributions[name].append_child() << t;
              new_node["value"] << this->inja_environment.render(t["value"].val<std::string>().value(), temp_tree.rootref());
            }
          } else if (t["value"].is_map()) {
            for (auto i = instance_names.first; i != instance_names.second; ++i) {
              ryml::Tree temp_tree;
              temp_tree["instance"] << i->second;
              auto new_node = t.duplicate(template_contributions[name], template_contributions[name].last_child());
              for (auto child: new_node["value"].children()) {
                if (child.has_val())
                  new_node["value"][child.key()] << this->inja_environment.render(child.val<std::string>().value(), temp_tree.rootref());
              }
            }
          } else {
            t.duplicate(template_contributions[name], template_contributions[name].last_child());
            // template_contributions[name].push_back(t);
          }
        } else {
          t.duplicate(template_contributions[name], template_contributions[name].last_child());
          // template_contributions[name].push_back(t);
        }
      }
    }

    // Process config_file
    if (c->root.contains("config_file")) {
      for (auto config: c->root["config_file"]) {
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
            create_config_file(c, config, instance_prefix, ryml_string(i->second));
        else
          create_config_file(c, config, instance_prefix, instance_prefix);
      }
    }

    // Process template_file
    if (c->root.contains("template_file")) {
      for (auto t: c->root["template_file"]) {
        if (is_disqualified_by_unless(t) || !condition_is_fulfilled(t))
          continue;

        std::filesystem::path template_file = t["path"].val<std::string>().value();
        std::filesystem::path target_file   = template_file.filename();
        target_file.replace_extension();

        auto target = "{{project_output}}/generated/" + target_file.string();

        if (target_file.extension() == ".c" || target_file.extension() == ".cpp")
          c->root["generated"]["sources"].append_child() << target;
        else if (target_file.extension() == ".h" || target_file.extension() == ".hpp")
          c->root["generated"]["includes"].append_child() << target;
        else if (target_file.extension() == ".ld")
          c->root["generated"]["linker_script"].append_child() << target;
        else
          c->root["generated"]["files"].append_child() << target;

        // Create blueprints
        auto blueprint = c->root["blueprints"].append_child() << ryml::key(target);
        blueprint |= ryml::MAP;
        blueprint["depends"] |= ryml::SEQ;
        blueprint["process"] |= ryml::SEQ;
        blueprint["depends"].append_child() << c->root["directory"].val<std::string>().value() + "/" + template_file.string();
        blueprint["depends"].append_child() << "{{project_output}}/template_contributions.json";
        blueprint["process"].append_child() << ryml::key("jinja") << "-t " + c->root["directory"].val<std::string>().value() + "/" + template_file.string() + " -d {{project_output}}/template_contributions.json";
        blueprint["process"].append_child() << ryml::key("save");

        // c->root["blueprints"][target] = blueprint;
      }
    }

    // Process special toolchain settings
    if (c->root.contains("toolchain_settings")) {
      for (const auto &s: c->root["toolchain_settings"]) {
        if (s["option"] == "linkerfile") {
          if (is_disqualified_by_unless(s) || !condition_is_fulfilled(s))
            continue;

          c->root["generated"]["linker_script"] << "{{project_output}}/generated/" + std::filesystem::path{ s["value"].val<std::string>().value() }.filename().string();
        }
      }
    }
  }

  // Process toolchain settings
  project_summary["toolchain_settings"] |= ryml::MAP;
  for (const auto &c: components) {
    if (c->root.contains("toolchain_settings") == false)
      continue;

    for (const auto &s: c->root["toolchain_settings"]) {
      if (is_disqualified_by_unless(s) || !condition_is_fulfilled(s))
        continue;

      const auto key = s["option"].val();
      if (project_summary["toolchain_settings"].contains(key))
        if (project_summary["toolchain_settings"][key].is_seq())
          project_summary["toolchain_settings"][key].append_child() << s["value"].val();
        else {
          auto current_value = project_summary["toolchain_settings"][key].val();
          project_summary["toolchain_settings"][key].set_type(ryml::SEQ);
          project_summary["toolchain_settings"][key].append_child() << current_value;
          project_summary["toolchain_settings"][key].append_child() << s["value"].val();
        }
      else
        project_summary["toolchain_settings"][key] << s["value"].val();
    }
  }

  // Go through the template_contributions and sort via priorities
  // TODO: Implement ryml version - local json variables for sorting
  ryml::NodeRef new_contributions = template_contributions.append_child() << ryml::key("sorted");
  new_contributions |= ryml::MAP;
  for (auto item: template_contributions.children()) {
    while (item.valid() && item.num_children() > 0) {
      // Remove the item with the lowest priority
      int lowest_priority       = INT_MAX;
      int lowest_priority_index = 0;
      for (size_t i = 0; i < item.num_children(); ++i) {
        int priority = item[i].contains("priority") ? item[i]["priority"].val<int>().value() : 0;
        if (priority < lowest_priority) {
          lowest_priority       = priority;
          lowest_priority_index = i;
        }
      }
      auto entry = item[lowest_priority_index];
      spdlog::info("Ordering '{}' at priority {}", entry["name"].val<std::string>().value(), lowest_priority);

      //const std::string value = entry["value"].val();
      new_contributions[entry["name"].val()].append_child() << entry["value"].val();
      item.remove_child(lowest_priority_index);
    }
  }
  template_contributions = new_contributions;
}

/// @brief Executes process_blueprints.

void project::process_blueprints(const std::shared_ptr<component> c)
{
  if (c->root.contains("blueprints") and c->root["blueprints"].has_children()) {

    for (auto b: c->root["blueprints"].children()) {
      std::string blueprint_string = try_render(inja_environment, b.has_child("regex") ? b["regex"].val() : b.key(), project_summary);
      if (blueprint_string.empty()) {
        spdlog::error("Invalid empty blueprint generated from component '{}'", c->id);
        continue;
      }
      if (blueprint_string[0] == data_dependency_identifier && !blueprint_string.starts_with(":/data/")) {
        spdlog::error("Invalid data blueprint: {}", blueprint_string);
        continue;
      }
      spdlog::info("Additional blueprint: {}", blueprint_string);
      try {
        // ryml::Tree blueprint_tree = ryml::parse_in_arena(b.val());
        blueprint_database.create_blueprint(blueprint_string, b, c->root["directory"].val());
        // auto new_blueprint        = std::make_shared<blueprint>(c4::to_csubstr(blueprint_string), b, c->root["directory"].val());
        // blueprint_database.blueprints.insert({ c4::to_csubstr(blueprint_string), new_blueprint });
      } catch (const std::exception &e) {
        spdlog::error("Failed to convert blueprint '{}' to ryml: {}", blueprint_string, e.what());
      }
    }
  }
}

/// @brief Executes process_tools.

void project::process_tools(const std::shared_ptr<component> c)
{
  if (c->root.contains("tools")) {

    for (auto i: c->root["tools"].children()) {
      inja::Environment inja_env;
      /// @brief Executes add_callback.

      inja_env.add_callback("curdir", 0, [&c](inja::Arguments &args, ryml::NodeRef additional_data) {
        return additional_data["values"].append_child() << std::filesystem::absolute(c->component_path).string();
      });

      add_tool_entry(i, project_summary, inja_env, c->component_path);
    }
  }
}

void project::load_projects()
{
  if (!fs::exists(this->output_path / "blueprints.json"))
    return;

  blueprint_database.load(this->output_path / "blueprints.json");
}

void project::save_blueprints()
{
  blueprint_database.save(this->output_path / "blueprints.json");
}

void project::process_construction(indicators::ProgressBar &bar)
{
  (void)bar;
}

void project::add_additional_tool(const std::filesystem::path component_path)
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
