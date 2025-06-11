# Task Engine Design Document

## Overview
The Task Engine is a core component of Yakka that provides an abstraction layer over the TaskFlow C++ library. It manages the creation, scheduling, and execution of build tasks in a dependency-aware manner.

## Key Components

### `task_engine` Class
The main class responsible for managing the build process. It orchestrates task creation, dependency management, and execution flow.

### `construction_task` 
A structure that represents a single build task, containing:
- Task execution information
- Last modification timestamps
- Blueprint match data
- Task group information for progress tracking

### `task_engine_ui`
An interface for providing progress feedback during task execution, allowing for different UI implementations (like progress bars).

## Core Functions

### `init(task_complete_type task_complete_handler)`
Initializes the task engine with a completion handler callback.

### `create_tasks(const std::string target_name, tf::Task &parent, yakka::project &project)`
The heart of the task engine that:
- Creates task nodes for the dependency graph
- Handles both leaf nodes (files, data dependencies) and complex nodes (build targets)
- Manages task dependencies and relationships
- Sets up task execution callbacks

Key behaviors:
- Detects and handles data dependencies (targets starting with a special identifier)
- Manages file timestamps for change detection
- Creates task groups for progress tracking
- Sets up build command execution

### `run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project)`
Executes build commands for a target with:
- Template engine integration (using Inja)
- Support for both built-in and external tools
- Command output capture
- Error handling and reporting
- Performance timing

### `run_taskflow(yakka::project &project, task_engine_ui *ui)`
Orchestrates the entire build process:
1. Creates an executor with appropriate thread count
2. Sets up the task graph
3. Initializes progress tracking UI
4. Executes the task graph
5. Provides real-time progress updates

## Task Creation Process

1. **Dependency Check**
   - Checks if target was already processed
   - Establishes parent-child relationships for dependencies

2. **Task Type Determination**
   - Leaf nodes (files, data dependencies)
   - Complex nodes (build targets)

3. **Task Setup**
   - Associates data with tasks
   - Establishes execution callbacks
   - Sets up progress tracking

## Task Execution Model

The engine uses a pull-based model where:
- Tasks are executed when their dependencies are satisfied
- Timestamps are used for incremental builds
- Build abortion is possible through a shared flag

## Progress Tracking

- Tasks are grouped for logical organization
- Real-time progress updates
- Support for different UI implementations
- Counts of total and completed tasks per group

## Error Handling

- Command execution status tracking
- Build abortion on critical errors
- Error logging and reporting
- Exception handling for template processing

## Performance Considerations

- Parallel execution through TaskFlow
- Thread count optimization
- Timestamp-based incremental builds
- Progress tracking with minimal overhead

## Integration Points

- Project configuration
- Blueprint system
- Template engine
- Command execution
- UI feedback system

## Usage Example

```cpp
yakka::task_engine engine;
progress_bar_task_ui ui;

// Initialize engine
engine.init(completion_handler);

// Configure project
yakka::project project(...);

// Execute build
engine.run_taskflow(project, &ui);
```

## Future Considerations

- Enhanced error reporting
- More granular progress tracking
- Extended template capabilities
- Additional built-in commands
- Performance optimizations for large builds
