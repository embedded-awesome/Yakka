#include "build_request.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <tuple>

namespace yakka {
namespace {

struct variant {
  std::vector<std::string> arguments;
  std::vector<std::string> alternatives;
};

auto request_identity(const std::vector<std::string> &arguments)
{
  std::vector<std::string> components;
  std::vector<std::string> features;
  std::vector<std::string> commands;
  for (const auto &argument: arguments) {
    if (argument.front() == '+')
      features.push_back(argument);
    else if (argument.back() == '!')
      commands.push_back(argument);
    else
      components.push_back(argument);
  }
  std::sort(commands.begin(), commands.end());
  commands.erase(std::unique(commands.begin(), commands.end()), commands.end());
  return std::tuple{ components, features, commands };
}

std::string generated_name(const std::vector<std::string> &arguments)
{
  std::string components;
  std::string features;
  for (const auto &argument: arguments) {
    if (argument.front() == '+')
      features += argument;
    else if (argument.back() != '!') {
      if (!components.empty())
        components += '-';
      components += argument;
    }
  }
  return components + features;
}

bool valid_name(const std::string &name)
{
  return name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos &&
         std::none_of(name.begin(), name.end(), [](unsigned char character) { return std::iscntrl(character); });
}

} // namespace

std::expected<std::vector<build_request>, std::string> expand_build_requests(
  const std::vector<std::string> &arguments, const std::string &project_name)
{
  constexpr size_t maximum_variants = 4096;
  std::vector<variant> variants(1);
  for (size_t index = 0; index < arguments.size(); ++index) {
    const auto &argument = arguments[index];
    if (argument.empty())
      return std::unexpected("Empty build argument");
    if (argument.front() != '[') {
      if (argument.find_first_of("[]") != std::string::npos)
        return std::unexpected("Unexpected bracket in build argument: " + argument);
      for (auto &entry: variants)
        entry.arguments.push_back(argument);
      continue;
    }

    std::string array = argument;
    while (array.back() != ']' && index + 1 < arguments.size()) {
      array += ' ';
      array += arguments[++index];
    }
    if (array.back() != ']')
      return std::unexpected("Unclosed build alternatives: " + array);

    std::vector<std::string> alternatives;
    try {
      const auto documents = YAML::LoadAll(array);
      if (documents.size() != 1)
        return std::unexpected("Build alternatives must contain exactly one sequence: " + array);
      const auto sequence = documents.front();
      if (!sequence.IsSequence() || sequence.size() == 0)
        return std::unexpected("Build alternatives must be a nonempty sequence: " + array);
      for (const auto &element: sequence) {
        if (!element.IsScalar())
          return std::unexpected("Build alternatives must contain only components or +features: " + array);
        auto selection = element.Scalar();
        if (selection.empty() || selection == "+" || selection.back() == '!' ||
            selection.find_first_of("[]") != std::string::npos ||
            std::any_of(selection.begin(), selection.end(), [](unsigned char character) { return std::isspace(character); }))
          return std::unexpected("Invalid build alternative: " + selection);
        if (std::find(alternatives.begin(), alternatives.end(), selection) == alternatives.end())
          alternatives.push_back(std::move(selection));
      }
    } catch (const YAML::Exception &error) {
      return std::unexpected("Invalid build alternatives: " + std::string(error.what()));
    }
    if (alternatives.size() > maximum_variants / variants.size())
      return std::unexpected("Build matrix exceeds 4096 projects");

    std::vector<variant> expanded;
    for (const auto &entry: variants)
      for (const auto &selection: alternatives) {
        auto next = entry;
        next.arguments.push_back(selection);
        next.alternatives.push_back(selection);
        expanded.push_back(std::move(next));
      }
    variants = std::move(expanded);
  }

  std::vector<build_request> requests;
  std::map<std::string, std::vector<std::string>> output_claims;
  for (const auto &entry: variants) {
    auto name = project_name.empty() ? generated_name(entry.arguments) : project_name;
    if (!project_name.empty() && variants.size() > 1)
      for (const auto &selection: entry.alternatives)
        name += (selection.front() == '+' ? "" : "-") + selection;
    if (!valid_name(name))
      return std::unexpected("Invalid project name: " + name);
    const auto [claim, inserted] = output_claims.emplace(name, entry.arguments);
    if (!inserted) {
      if (request_identity(claim->second) != request_identity(entry.arguments))
        return std::unexpected("Different build requests claim the same project output: " + name);
      continue;
    }
    requests.push_back({ entry.arguments, std::move(name) });
  }
  return requests;
}

} // namespace yakka