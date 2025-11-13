# Yakka code and object structure

Yakka has a number of key concepts; workspace, project, component, blueprints, and actions.

The workspace represents a directory path that acts as the root for Yakka actions. It can hold a config file (config.yaml), the .yakka folder, and acts as the base for finding other components.

A project represents a particular set of components, and features that operate from within a workspace.
An instantiation of Yakka may involve multiple projects.
Projects generally require an evaluation process that identifies dependencies and attempts to resolve them. The evaluation process can be interactive whereby the project owner (typically the Yakka CLI) can perform actions such as downloading missing components or prompting the user for addtional information.


# Project evaluation process

- CLI creates project. Provides initial set of components and features
- CLI gets project to evaluate itself.
- Project sets internal state, lists any missing components, features or choices.
- CLI inspects project and reacts.
  - If missing components and user has indicated to download, request workspace to fetch components
  - If missing choices, prompt user to make a choice
- If any actions have occurred in response to project state, get project to evaluate

