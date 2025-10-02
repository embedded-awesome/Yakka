#include "yakka_cli_actions.hpp"
#include <indicators/dynamic_progress.hpp>
#include <indicators/progress_bar.hpp>
#include <indicators/cursor_control.hpp>
#include <indicators/termcolor.hpp>

using namespace indicators;
using namespace std::chrono_literals;

namespace yakka {

int register_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  if (result.unmatched().size() == 0) {
    spdlog::error("Must provide URL of component registry");
    return -1;
  }
  // Ensure the BOB registries directory exists
  std::filesystem::create_directories(".yakka/registries");
  spdlog::info("Adding component registry...");
  auto status = workspace.add_component_registry(result.unmatched()[0]);
  if (!status.has_value()) {
    spdlog::error("Failed to add component registry. See yakka.log for details: {}", status.error().message());
    return -1;
  }
  spdlog::info("Complete");
  return 0;
}

int list_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  workspace.load_component_registries();
  for (auto registry: workspace.registries) {
    // Group components by type
    std::multimap<std::string, std::string> components_by_type;
    for (auto c: registry.second["provides"]["components"])
      components_by_type.insert({c.second["type"] ? c.second["type"].as<std::string>() : "component", c.first.as<std::string>()});

    // Print registry name
    std::cout << registry.second["name"] << "\n";

    // Print components grouped by type
    for (auto it = components_by_type.begin(); it != components_by_type.end(); ) {
        const std::string& type = it->first;
        std::cout << "type: " << type << "\n";

        // Get the range of elements with this key
        auto range = components_by_type.equal_range(type);
        for (auto c = range.first; c != range.second; ++c) {
            std::cout << "  - " << c->second << "\n";
        }

        // Advance iterator to the next key group
        it = range.second;
    }
  }
  return 0;
}

int update_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  for (auto &i: result.unmatched()) {
    std::cout << "Updating: " << i << "\n";
    auto result = workspace.update_component(i);
    if (!result.has_value()) {
      spdlog::error("Failed to update component '{}'. See yakka.log for details: {}", i, result.error().message());
      return -1;
    }
  }
  std::cout << "Complete\n";
  return 0;
}

int remove_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  for (auto &i: result.unmatched()) {
    auto optional_location = workspace.find_component(i);
    if (optional_location) {
      auto [path, package] = optional_location.value();
      spdlog::info("Removing {}", path.string());
      std::filesystem::remove_all(path);
    }
  }
  std::cout << "Complete\n";
  return 0;
}

int git_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  auto iter                 = result.unmatched().begin();
  const auto component_name = *iter;
  std::string git_command   = "--git-dir=.yakka/repos/" + component_name + "/.git --work-tree=components/" + component_name;
  for (iter++; iter != result.unmatched().end(); ++iter)
    if (iter->find(' ') == std::string::npos)
      git_command.append(" " + *iter);
    else
      git_command.append(" \"" + *iter + "\"");

  auto [output, result_code] = yakka::exec("git", git_command);
  std::cout << output;
  return 0;
}

int fetch_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  yakka::project project("", workspace);
  // Identify components named on command line and add to unknown components
  for (auto s: result.unmatched()) {
    if (s.front() == '+' || s.back() == '!')
      continue;
    else
      project.unknown_components.insert(s);
  }

  // Fetch the components
  download_unknown_components(workspace, project);
  return 0;
}

int serve_action(workspace &workspace, const cxxopts::ParseResult &result)
{
  spdlog::info("Starting configuration server...");
  bool server_running = false;
  yakka::start_config_server(workspace, server_running);
  return 0;
}

// clang-format off
// const std::unordered_map<std::string, action_handler> cli_actions = { 
//   { "register", register_action }, 
//   { "list", list_action },   
//   { "update", update_action }, 
//   { "remove", remove_action },
//   { "git", git_action },           
//   { "fetch", fetch_action }, 
//   { "serve", serve_action } 
// };
// clang-format on


void download_unknown_components(yakka::workspace &workspace, yakka::project &project)
{
  auto t1 = std::chrono::high_resolution_clock::now();

  // If there are still missing components, try and download them
  if (!project.unknown_components.empty()) {
    workspace.load_component_registries();

    show_console_cursor(false);
    DynamicProgress<ProgressBar> fetch_progress_ui;
    std::map<std::string, std::shared_ptr<ProgressBar>> fetch_progress_bars;

    std::map<std::string, std::future<fs::path>> fetch_list;
    int largest_name_length = 16;
    do {
      // Ask the workspace to fetch them
      for (const auto &i: project.unknown_components) {
        if (fetch_list.find(i) != fetch_list.end())
          continue;

        // Check if component is in the registry
        auto node = workspace.find_registry_component(i);
        if (node) {
          auto prefix_test = "Fetching " + i + " ";
          if (prefix_test.size() > largest_name_length) {
            largest_name_length = prefix_test.size();
          }
          if (prefix_test.size() < largest_name_length)
            prefix_test.append(largest_name_length - prefix_test.size(), ' ');
          else if (prefix_test.size() > largest_name_length)
            prefix_test = prefix_test.substr(0, largest_name_length);

          std::shared_ptr<ProgressBar> new_progress_bar = std::make_shared<ProgressBar>(option::BarWidth{ 50 }, option::ShowPercentage{ true }, option::PrefixText{ prefix_test }, option::SavedStartTime{ true });
          fetch_progress_bars.insert({i, new_progress_bar});
          size_t id = fetch_progress_ui.push_back(*new_progress_bar);
          
          auto result = workspace.fetch_component(i, *node, [&fetch_progress_ui, &largest_name_length, id, i](std::string_view postfix, size_t number) {
            fetch_progress_ui[id].set_option(option::PostfixText{ postfix });
            auto prefix_test = "Fetching " + i + " ";
            if (prefix_test.size() < largest_name_length) {
              prefix_test.append(largest_name_length - prefix_test.size(), ' ');
              fetch_progress_ui[id].set_option(option::PrefixText{ prefix_test });
            }
            if (number >= 100) {
              fetch_progress_ui[id].set_progress(100);
              fetch_progress_ui[id].mark_as_completed();
            } else
              fetch_progress_ui[id].set_progress(number);
          });

          if (result.valid())
            fetch_list.insert({ i, std::move(result) });
        }
      }

      // Check if we haven't been able to fetch any of the unknown components
      if (fetch_list.empty()) {
        for (const auto &i: project.unknown_components)
          spdlog::error("Cannot fetch {}", i);
        spdlog::shutdown();
        exit(0);
      }

      // Wait for one of the components to be complete
      decltype(fetch_list)::iterator completed_fetch;
      do {
        completed_fetch = std::find_if(fetch_list.begin(), fetch_list.end(), [](auto &fetch_item) {
          return fetch_item.second.wait_for(100ms) == std::future_status::ready;
        });
      } while (completed_fetch == fetch_list.end());

      auto new_component_path = completed_fetch->second.get();

      // Check if the fetch worked
      if (new_component_path.empty()) {
        spdlog::error("Failed to fetch {}", completed_fetch->first);
        project.unknown_components.erase(completed_fetch->first);
        fetch_list.erase(completed_fetch);
        continue;
      }

      // Update the component database
      if (new_component_path.string().starts_with(workspace.shared_components_path.string())) {
        spdlog::info("Scanning for new component in shared database");
        workspace.shared_database.scan_for_components(new_component_path);
        auto result = workspace.shared_database.save();
        if (!result.has_value()) {
          spdlog::error("Failed to save shared database: {}", result.error().message());
          exit(1);
        }
      } else {
        spdlog::info("Scanning for new component in local database");
        workspace.local_database.scan_for_components(new_component_path);
        auto result = workspace.shared_database.save();
        if (!result.has_value()) {
          spdlog::error("Failed to save shared database: {}", result.error().message());
          exit(1);
        }
      }

      // Check if any of our unknown components have been found
      for (auto i = project.unknown_components.cbegin(); i != project.unknown_components.cend();) {
        if (!workspace.local_database.get_component(*i, project.component_flags).has_value() || !workspace.shared_database.get_component(*i, project.component_flags).has_value()) {
          // Remove component from the unknown list and add it to the unprocessed list
          project.unprocessed_components.insert(*i);
          i = project.unknown_components.erase(i);
        } else
          ++i;
      }

      // Remove the item from the fetch list
      fetch_list.erase(completed_fetch);

      // Re-evaluate the project dependencies
      project.evaluate_dependencies();
    } while (!project.unprocessed_components.empty() || !project.unknown_components.empty() || !fetch_list.empty());
  }

  auto t2       = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to download missing components", duration);
}


} // namespace yakka