#include "yakka_blueprint.hpp"
#include "utilities.hpp"
#include <iostream>

namespace yakka {
blueprint::blueprint(const c4::csubstr &target, ryml::ConstNodeRef blueprint_data, const c4::csubstr &parent_path)
{
  this->target      = target;
  this->parent_path = parent_path;
  this->data        = blueprint_data;
  this->process     = ryml::ConstNodeRef();

  const auto root = data;

  if (root.has_child("regex"))
    this->regex = root["regex"].val();

  if (const auto requires = root["requires"]; requires.valid() && requires.is_seq()) {
    for (const auto &d: requires.children())
      this->requirements.push_back(d.val());
  }

  if (const auto depends = root["depends"]; depends.valid() && depends.is_seq()) {
    for (const auto &d: depends.children()) {
      if (!d.is_map() && d.has_val()) {
        const auto dep_value = d.val();
        if (!dep_value.empty() && dep_value.front() == ':')
          this->dependencies.push_back({ dependency::DATA_DEPENDENCY, dep_value });
        else
          this->dependencies.push_back({ dependency::DEFAULT_DEPENDENCY, dep_value });
      } else if (d.is_map()) {
        if (d.has_child("data")) {
          const auto data_node = d["data"];
          if (data_node.is_seq())
            for (const auto &i: data_node.children())
              this->dependencies.push_back({ dependency::DATA_DEPENDENCY, i.val() });
          else
            this->dependencies.push_back({ dependency::DATA_DEPENDENCY, data_node.val() });
        } else if (d.has_child("dependency_file")) {
          this->dependencies.push_back({ dependency::DEPENDENCY_FILE_DEPENDENCY, d["dependency_file"].val() });
        }
      }
    }
  }

  if (root.has_child("process"))
    process = root["process"];

  if (root.has_child("group"))
    this->task_group = root["group"].val();
}

} // namespace yakka