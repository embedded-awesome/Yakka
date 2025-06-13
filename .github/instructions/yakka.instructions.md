# Project Context for GitHub Copilot

This document provides context for GitHub Copilot about the Yakka project.

## Project Overview
Yakka is a build system and development tool designed to develop embedded software. The name "Yakka" is derived from Australian slang meaning "work", especially hard work.

## Key Components
- `yakka_cli`: The main command-line interface
- `yakka_blueprint`: Handles project blueprints
- `yakka_component`: Manages project components
- `yakka_workspace`: Manages workspace configuration
- `task_engine`: Handles build task execution
- `template_engine`: Processes templates
- `component_database`: Manages component dependencies

## Project Structure
- `/yakka`: Core source files containing the main implementation
- `/components`: External dependencies and components (cpp-semver, cryptopp, fmt, etc.)
- `/docs`: Project documentation including design docs and blueprints
- `/test`: Unit tests for the project
- `/tools`: Build tools and utilities

## Build System
The project uses a custom build system. Build configurations are managed through .yakka files that define component structure and dependencies.

## Technical Details
- Language: Modern C++ (C++23)
- Build System: Yakka
- Platform Support: Linux, macOS, Windows
- Key External Dependencies:
  - FTXUI for terminal UI
  - fmt for string formatting
  - spdlog for logging
  - nlohmann/json for JSON handling
  - yaml-cpp for YAML parsing
  - Various testing and utility libraries

## Coding Standards
- Use modern C++ features and idioms
- Follow consistent naming conventions:
  - Classes: PascalCase (e.g., `TaskEngine`)
  - Methods/Functions: camelCase (e.g., `executeTask`)
  - Variables: snake_case (e.g., `component_name`)
- Write comprehensive error handling
- Include documentation comments for public interfaces
- Use YAML for configuration files
- Follow component-based architecture principles

## Common Development Tasks
- Building: Use `yakka-macos link! yakka_cli xcode` on macOS, `yakka-windows link! yakka_cli msvc` on Windows
- Testing: Unit tests are in the `/test` directory
- Component Management: Components are defined in .yakka files
- Blueprint System: Used for project templates and generation

Note: This context file helps GitHub Copilot provide more accurate and project-specific suggestions. Keep this file updated as the project evolves.