#include "yakka_blueprint.hpp"
#include "utilities.hpp"
#include <iostream>

namespace yakka {
blueprint::blueprint(const std::string &target, ryml::Tree blueprint_data, const std::string &parent_path)
{
  this->target      = target;
  this->parent_path = parent_path;
  this->data        = std::move(blueprint_data);
  this->process     = ryml::ConstNodeRef();

  const auto root = data.crootref();

  if (root.has_child("regex"))
    this->regex = ryml_get_val_as_string(root["regex"]);

  if (root.has_child("requires")) {
    const auto requires = root["requires"];
    if (requires.is_seq()) {
      for (const auto &d: requires.children())
        this->requirements.push_back(ryml_get_val_as_string(d));
    }
  }

  if (root.has_child("depends")) {
    const auto depends = root["depends"];
    if (depends.is_seq())
      for (const auto &d: depends.children()) {
        if (!d.is_map() && d.has_val()) {
          const auto dep_value = ryml_get_val_as_string(d);
          if (!dep_value.empty() && dep_value.front() == ':')
            this->dependencies.push_back({ dependency::DATA_DEPENDENCY, dep_value });
          else
            this->dependencies.push_back({ dependency::DEFAULT_DEPENDENCY, dep_value });
        } else if (d.is_map()) {
          if (d.has_child("data")) {
            const auto data_node = d["data"];
            if (data_node.is_seq())
              for (const auto &i: data_node.children())
                this->dependencies.push_back({ dependency::DATA_DEPENDENCY, ryml_get_val_as_string(i) });
            else
              this->dependencies.push_back({ dependency::DATA_DEPENDENCY, ryml_get_val_as_string(data_node) });
          } else if (d.has_child("dependency_file")) {
            this->dependencies.push_back({ dependency::DEPENDENCY_FILE_DEPENDENCY, ryml_get_val_as_string(d["dependency_file"]) });
          }
        }
      }
  }

  if (root.has_child("process"))
    process = root["process"];

  if (root.has_child("group"))
    this->task_group = ryml_get_val_as_string(root["group"]);
}

const ryml::Tree &blueprint::as_ryml() const
{
  return data;
}
} // namespace yakka