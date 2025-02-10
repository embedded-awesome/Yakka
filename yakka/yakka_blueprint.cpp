#include "yakka_blueprint.hpp"
#include <iostream>

namespace yakka {
blueprint::blueprint(const std::string &target, const nlohmann::json &blueprint, const std::string &parent_path)
{
  this->target      = target;
  this->parent_path = parent_path;

  if (blueprint.contains("regex"))
    this->regex = blueprint["regex"].get<std::string>();

  if (blueprint.contains("requires"))
    for (auto &d: blueprint["requires"])
      this->requirements.push_back(d.get<std::string>());

  if (blueprint.contains("depends"))
    for (auto &d: blueprint["depends"]) {
      if (d.is_primitive())
        this->dependencies.push_back({ dependency::DEFAULT_DEPENDENCY, d.get<std::string>() });
      else if (d.is_object()) {
        if (d.contains("data")) {
          if (d["data"].is_array())
            for (auto &i: d["data"])
              this->dependencies.push_back({ dependency::DATA_DEPENDENCY, i.get<std::string>() });
          else
            this->dependencies.push_back({ dependency::DATA_DEPENDENCY, d["data"].get<std::string>() });
        } else if (d.contains("dependency_file")) {
          this->dependencies.push_back({ dependency::DEPENDENCY_FILE_DEPENDENCY, d["dependency_file"].get<std::string>() });
        }
      }
    }

  if (blueprint.contains("process"))
    process = blueprint["process"];

  if (blueprint.contains("group"))
    this->task_group = blueprint["group"].get<std::string>();
}

nlohmann::json blueprint::as_json() const
{
  nlohmann::json j;
  j["target"] = target;

  if (!regex.empty())
    j["regex"] = regex;

  if (!requirements.empty())
    j["requires"] = requirements;

  if (!dependencies.empty()) {
    nlohmann::json deps = nlohmann::json::array();
    for (const auto &dep: dependencies) {
      if (dep.type == dependency::DATA_DEPENDENCY) {
        nlohmann::json d;
        d["data"] = dep.name;
        deps.push_back(d);
      } else if (dep.type == dependency::DEPENDENCY_FILE_DEPENDENCY) {
        nlohmann::json d;
        d["dependency_file"] = dep.name;
        deps.push_back(d);
      } else {
        deps.push_back(dep.name);
      }
    }
    j["depends"] = deps;
  }

  if (!process.empty())
    j["process"] = process;

  if (!task_group.empty())
    j["group"] = task_group;

  return j;
}
} // namespace yakka