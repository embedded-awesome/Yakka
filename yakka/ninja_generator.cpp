#include "ninja_generator.hpp"
#include "blueprint_commands.hpp"
#include "project_diff.hpp"
#include "task_engine.hpp"
#include "utilities.hpp"
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"
#include "glob/glob.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace yakka {
namespace {

enum class node_kind {
  source,         // Existing file with no blueprint
  missing_source, // Neither a file nor a blueprint; always out of date, like the legacy engine
  data,           // ':' data dependency, folded into command hashes
  phony,          // Blueprint without a process
  generated,      // Evaluated during generation; becomes an implicit output of the regeneration edge
  virtual_output, // Evaluated during generation but produced no file (e.g. saved to project data)
  shell,          // Native Ninja edge running the rendered command
  yakka,          // Ninja edge that calls back into `yakka --ninja-exec`
};

struct node_info {
  node_kind kind = node_kind::source;
  bool visiting  = false;
  std::string command;
  std::string description;
  std::string depfile;
  std::vector<std::string> inputs;
  std::vector<std::string> data_inputs;
};

// Built-in commands that only transform project data and files, so they can run while generating the manifest.
const std::set<std::string, std::less<>> generate_time_commands = { "inja", "template", "save", "regex", "as_json", "as_yaml", "cat", "create_directory" };

std::string normalize_target(std::string_view target)
{
  if (target.starts_with("./")) {
    const auto first = target.find_first_not_of('/', 2);
    target           = first == std::string_view::npos ? std::string_view{} : target.substr(first);
  }
  return std::string(target);
}

ryml::ConstNodeRef resolve_data_pointer(ryml::ConstNodeRef node, std::string_view data_path)
{
  // Data dependencies look like ":/components/foo/bar"
  size_t position = data_path.find('/');
  while (position != std::string_view::npos && node.valid()) {
    const auto next  = data_path.find('/', position + 1);
    const auto token = data_path.substr(position + 1, next == std::string_view::npos ? std::string_view::npos : next - position - 1);
    position         = next;
    if (token.empty())
      continue;
    const auto key = ryml::csubstr(token.data(), token.size());
    if (node.is_map()) {
      node = node.find_child(key);
    } else if (node.is_seq()) {
      size_t index      = 0;
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
      if (parsed.ec != std::errc{} || index >= node.num_children())
        return {};
      node = node[index];
    } else {
      return {};
    }
  }
  return node;
}

std::string data_hash(const project &project, const std::vector<std::string> &data_inputs)
{
  std::string serialized;
  for (const auto &input: data_inputs) {
    serialized += input;
    serialized += '=';
    const auto node = resolve_data_pointer(ryml::ConstNodeRef(project.project_summary), input);
    if (node.valid() && node.id() != ryml::NONE)
      serialized += node.is_keyval() || node.is_val() ? ryml_string(node.val()) : ryml::emitrs_json<std::string>(node);
    serialized += '\n';
  }
  return project_diff::to_hex(project_diff::hash_bytes(serialized.data(), serialized.size()));
}

std::string display_path(const std::string &path)
{
  std::error_code ec;
  const auto relative = fs::path(path).lexically_relative(fs::current_path(ec));
  if (!ec && !relative.empty() && *relative.begin() != "..")
    return relative.generic_string();
  return path;
}

std::string glob_root(const std::string &pattern)
{
  const auto wildcard = pattern.find_first_of("*?[");
  const auto prefix   = pattern.substr(0, wildcard);
  const auto slash    = prefix.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : prefix.substr(0, slash == 0 ? 1 : slash);
}

class generator {
public:
  generator(project &project, const workspace &workspace) : project_(project), workspace_(workspace) {}

  std::expected<void, std::string> visit(const std::string &raw_name)
  {
    const auto name = normalize_target(raw_name);
    if (name.empty())
      return {};
    if (const auto found = nodes_.find(name); found != nodes_.end()) {
      if (found->second.visiting)
        return std::unexpected("Circular blueprint dependency at " + name);
      return {};
    }
    auto &node = nodes_[name];
    if (name.front() == data_dependency_identifier) {
      node.kind = node_kind::data;
      return {};
    }

    node.visiting      = true;
    const auto entry   = project_.target_database.targets.find(ryml::to_csubstr(name));
    const auto *matches = entry == project_.target_database.targets.end() ? nullptr : &entry->second;
    if (matches)
      for (const auto &match: *matches)
        if (match)
          for (const auto &dependency: match->dependencies)
            if (auto result = visit(ryml_string(dependency)); !result)
              return result;

    auto result   = classify(name, matches);
    // nodes_ may have rehashed during recursion, so look the node up again.
    nodes_[name].visiting = false;
    if (!result)
      return result;
    order_.push_back(name);
    return {};
  }

  std::expected<void, std::string> write(const fs::path &manifest_path, const ninja_generator_options &options)
  {
    std::ostringstream out;
    const auto output_dir = display_path(project_.output_path.generic_string());
    const auto manifest   = display_path(manifest_path.generic_string());
    out << "# Generated by Yakka for project '" << project_.project_name << "'. Do not edit.\n";
    out << "ninja_required_version = 1.5\n";
    out << "builddir = " << ninja_escape_value(output_dir) << "\n";
    out << "yakka = " << ninja_escape_value(shell_quote(options.yakka_binary)) << "\n";
    out << "yakka_args = " << ninja_escape_value(options.yakka_arguments) << "\n";
    if (!options.projects.empty())
      out << "yakka_projects = " << ninja_escape_value(options.projects) << "\n";
    out << "\n";

    out << "rule yakka_regenerate\n"
           "  command = $yakka --ninja-regenerate $yakka_args\n"
           "  description = Regenerating "
        << ninja_escape_value(manifest)
        << "\n"
           "  generator = 1\n"
           "  restat = 1\n"
           "  pool = console\n\n";
    out << "rule shell\n  command = $cmd\n  description = $desc\n\n";
    out << "rule shell_deps\n  command = $cmd\n  description = $desc\n  depfile = $depfile\n  deps = gcc\n\n";
    out << "rule yakka\n  command = $yakka --ninja-exec $target $yakka_args$data\n  description = $desc\n  restat = 1\n\n";
    out << "rule yakka_deps\n  command = $yakka --ninja-exec $target $yakka_args$data\n  description = $desc\n  restat = 1\n  depfile = $depfile\n  deps = gcc\n\n";

    std::set<std::string> outputs;
    std::vector<std::string> generated_outputs;
    for (const auto &name: order_) {
      const auto &node = nodes_.at(name);
      if (node.kind == node_kind::generated)
        generated_outputs.push_back(name);
      if (node.kind == node_kind::generated || node.kind == node_kind::shell || node.kind == node_kind::yakka || node.kind == node_kind::phony || node.kind == node_kind::missing_source)
        outputs.insert(fs::absolute(name).lexically_normal().generic_string());
    }

    for (const auto &name: order_) {
      const auto &node = nodes_.at(name);
      switch (node.kind) {
        case node_kind::phony:
        case node_kind::missing_source:
          out << "build " << ninja_escape_path(name) << ": phony";
          write_inputs(out, node.inputs);
          out << "\n";
          break;
        case node_kind::shell:
        case node_kind::yakka: {
          const bool shell = node.kind == node_kind::shell;
          out << "build " << ninja_escape_path(name) << ": " << (shell ? "shell" : "yakka") << (node.depfile.empty() ? "" : "_deps");
          write_inputs(out, node.inputs);
          out << "\n";
          std::string data;
          if (!node.data_inputs.empty())
            data = " # data " + data_hash(project_, node.data_inputs);
          if (shell)
            out << "  cmd = " << ninja_escape_value(node.command + data) << "\n";
          else {
            out << "  target = " << ninja_escape_value(shell_quote(name)) << "\n";
            if (!data.empty())
              // Ninja strips leading whitespace from values, so escape the space that separates the shell comment.
              out << "  data = $" << ninja_escape_value(data) << "\n";
          }
          out << "  desc = " << ninja_escape_value(node.description) << "\n";
          if (!node.depfile.empty())
            out << "  depfile = " << ninja_escape_value(node.depfile) << "\n";
          break;
        }
        default:
          break;
      }
    }

    // Regeneration inputs: everything the project summary, blueprints and generate-time targets were derived from.
    std::set<std::string> regen_inputs;
    const auto add_input = [&](const fs::path &path) {
      if (path.empty())
        return;
      std::error_code ec;
      if (!fs::exists(path, ec))
        return;
      const auto absolute = fs::absolute(path).lexically_normal().generic_string();
      if (outputs.contains(absolute) || absolute == fs::absolute(manifest_path).lexically_normal().generic_string())
        return;
      regen_inputs.insert(display_path(absolute));
    };
    add_input(project_.project_file);
    add_input(workspace_.workspace_path / "config.yaml");
    add_input(workspace_.local_database.get_path() / database_filename);
    add_input(workspace_.shared_database.get_path() / database_filename);
    for (const auto &database: workspace_.package_databases)
      add_input(database.get_path() / database_filename);
    for (const auto &component: project_.components)
      add_input(component->file_path);
    for (const auto &path: project_.template_inputs.files)
      add_input(path);
    for (const auto &path: project_.blueprint_database.template_inputs.files)
      add_input(path);
    auto patterns = project_.template_inputs.globs;
    patterns.insert(project_.blueprint_database.template_inputs.globs.begin(), project_.blueprint_database.template_inputs.globs.end());
    // Directory timestamps change when entries are added or removed, which approximates glob re-expansion.
    for (const auto &pattern: patterns) {
      for (const auto &argument: pattern)
        add_input(glob_root(argument));
      for (const auto &path: glob::rglob(pattern))
        add_input(path.parent_path());
    }
    for (const auto &name: order_) {
      const auto &node = nodes_.at(name);
      if (node.kind == node_kind::generated)
        for (const auto &input: node.inputs)
          add_input(input);
    }

    out << "\nbuild " << ninja_escape_path(manifest);
    if (!generated_outputs.empty()) {
      out << " |";
      for (const auto &output: generated_outputs)
        out << " " << ninja_escape_path(output);
    }
    out << ": yakka_regenerate";
    for (const auto &input: regen_inputs)
      out << " " << ninja_escape_path(input);
    out << "\n\ndefault";
    for (const auto &command: project_.commands)
      out << " " << ninja_escape_path(normalize_target(ryml_string(command)));
    out << "\n";

    std::error_code ec;
    fs::create_directories(manifest_path.parent_path(), ec);
    write_file_if_changed(manifest_path, out.str());
    return {};
  }

private:
  bool is_static(const std::string &name) const
  {
    const auto &node = nodes_.at(name);
    return node.kind == node_kind::source || node.kind == node_kind::data || node.kind == node_kind::generated || node.kind == node_kind::virtual_output;
  }

  std::expected<void, std::string> classify(const std::string &name, const std::vector<std::shared_ptr<blueprint_match>> *matches)
  {
    auto &node = nodes_.at(name);
    if (!matches || matches->empty()) {
      std::error_code ec;
      node.kind = fs::exists(name, ec) ? node_kind::source : node_kind::missing_source;
      if (node.kind == node_kind::missing_source)
        spdlog::info("[{}] '{}' has no blueprint and does not exist; it will always be out of date", project_.project_name, name);
      return {};
    }

    bool has_process       = false;
    bool generate_time     = true;
    size_t process_matches = 0;
    std::set<std::string> seen;
    for (const auto &match: *matches) {
      if (!match || !match->blueprint)
        continue;
      for (const auto &dependency: match->dependencies) {
        const auto dependency_name = normalize_target(ryml_string(dependency));
        if (dependency_name.empty() || !seen.insert(dependency_name).second)
          continue;
        const auto &dependency_node = nodes_.at(dependency_name);
        if (!is_static(dependency_name))
          generate_time = false;
        if (dependency_node.kind == node_kind::data)
          node.data_inputs.push_back(dependency_name);
        else if (dependency_node.kind != node_kind::virtual_output)
          node.inputs.push_back(dependency_name);
      }
      if (node.depfile.empty() && !match->dependency_files.empty()) {
        node.depfile = ryml_string(match->dependency_files.front());
        if (match->dependency_files.size() > 1)
          spdlog::warn("[{}] '{}' declares several dependency files; only '{}' is tracked", project_.project_name, name, node.depfile);
      }
      const auto process = match->blueprint->process;
      if (!process.valid() || !process.is_seq() || process.num_children() == 0)
        continue;
      has_process = true;
      ++process_matches;
      for (const auto step: process.children()) {
        if (!step.is_map() || step.num_children() != 1 || !generate_time_commands.contains(ryml_string(step.child(0).key()))) {
          generate_time = false;
          break;
        }
      }
    }

    if (!has_process) {
      node.kind = node_kind::phony;
      return {};
    }

    const auto *first_process = &matches->front();
    for (const auto &match: *matches)
      if (match && match->blueprint && match->blueprint->process.valid() && match->blueprint->process.num_children() > 0) {
        first_process = &match;
        break;
      }
    const auto &group = (*first_process)->blueprint->task_group;
    node.description  = (group.empty() ? std::string("YAKKA") : ryml_string(group)) + " " + display_path(name);

    if (generate_time) {
      task_engine engine;
      for (const auto &match: *matches) {
        if (!match || !match->blueprint || !match->blueprint->process.valid())
          continue;
        const auto [output, retcode] = engine.run_command(name, match, project_, project_.project_summary["data"]);
        if (retcode != 0)
          return std::unexpected("Failed to generate '" + name + "'");
      }
      std::error_code ec;
      node.kind = fs::exists(name, ec) ? node_kind::generated : node_kind::virtual_output;
      return {};
    }

    if (process_matches == 1)
      if (auto command = task_engine::render_shell_process(name, *first_process, project_)) {
        node.kind    = node_kind::shell;
        node.command = std::move(*command);
        if (group.empty()) {
          // Label the edge with the program it runs, e.g. "clang++ output/app/main.o"
          const auto start   = node.command.find_first_not_of("( ");
          const auto program = node.command.substr(start, node.command.find(' ', start) - start);
          node.description   = fs::path(program).filename().generic_string() + " " + display_path(name);
        }
        return {};
      }
    node.kind = node_kind::yakka;
    return {};
  }

  static void write_inputs(std::ostringstream &out, const std::vector<std::string> &inputs)
  {
    for (const auto &input: inputs)
      out << " " << ninja_escape_path(input);
  }

  project &project_;
  const workspace &workspace_;
  std::unordered_map<std::string, node_info> nodes_;
  std::vector<std::string> order_;
};

} // namespace

std::string ninja_escape_path(std::string_view path)
{
  std::string result;
  result.reserve(path.size());
  for (const auto c: path) {
    if (c == '$' || c == ' ' || c == ':')
      result += '$';
    result += c;
  }
  return result;
}

std::string ninja_escape_value(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const auto c: value) {
    if (c == '$')
      result += '$';
    result += c;
  }
  return result;
}

std::string shell_quote(std::string_view argument)
{
  if (!argument.empty() && argument.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-+=./,@%") == std::string_view::npos)
    return std::string(argument);
  std::string result = "'";
  for (const auto c: argument) {
    if (c == '\'')
      result += "'\\''";
    else
      result += c;
  }
  result += "'";
  return result;
}

std::expected<fs::path, std::string> generate_ninja_manifest(project &project, const workspace &workspace, const ninja_generator_options &options)
{
  const auto manifest_path = project.output_path / ninja_manifest_filename;
  const bool previous      = save_preserves_unchanged_files;
  save_preserves_unchanged_files = true;
  generator generator(project, workspace);
  std::expected<void, std::string> result;
  try {
    std::vector<std::string> commands;
    for (const auto &command: project.commands)
      commands.push_back(ryml_string(command));
    std::sort(commands.begin(), commands.end());
    for (const auto &command: commands)
      if (result = generator.visit(command); !result)
        break;
    if (result)
      result = generator.write(manifest_path, options);
  } catch (const std::exception &e) {
    result = std::unexpected(std::string(e.what()));
  }
  save_preserves_unchanged_files = previous;
  if (!result)
    return std::unexpected(result.error());
  return manifest_path;
}

} // namespace yakka
