# Component Database Design Document

## Overview
The Component Database is a core system in Yakka that manages the discovery, storage, and retrieval of build components and their metadata. It provides a persistent storage mechanism for component information, including their locations, blueprints, features, and type information.

## Key Concepts

### Components
- Building blocks of the system that can be discovered and loaded
- Can be defined in multiple file formats:
  - `.yakka` - Yakka native format
  - `.slcc` - SLC component format
  - `.slcp` - SLC project format
  - `.slce` - SLC extension format

### Database Structure
The database is stored as a JSON structure with the following main sections:
```json
{
    "blueprints": {},    // Blueprint definitions and providers
    "components": {},    // Component locations and metadata
    "features": {},      // Feature providers and conditions
    "types": {}         // Component type classifications
}
```

## Core Features

### Component Discovery
- Recursive scanning of workspace directories
- Support for multiple component file formats
- Permission-aware scanning (checks read permissions)
- File extension filtering
- Hidden file exclusion (files starting with '.')

### Component Identification
- Components are identified by unique IDs
- Multiple component files can map to the same ID
- Support for versioning and variants

### Feature Management
- Tracks which components provide specific features
- Supports conditional feature availability
- Allows feature querying for dependency resolution

### Blueprint Registry
- Maps blueprints to their provider components
- Excludes regex-based and templated blueprints
- Supports blueprint discovery and lookup

## Main Operations

### Database Management
- `load()`: Loads database from disk or creates new if none exists
- `save()`: Persists database to disk in JSON format
- `clear()`: Resets database to empty state
- `erase()`: Removes database file from disk

### Component Operations
- `insert()`: Adds component entry to database
- `add_component()`: Validates and adds component with metadata
- `get_component()`: Retrieves component path by ID
- `get_component_id()`: Looks up component ID from path

### File Parsing
- `parse_yakka_file()`: Processes Yakka format component files
- `parse_slcc_file()`: Processes SLC format component files
- Extracts:
  - Component metadata
  - Blueprint definitions
  - Feature provisions
  - Type information

### Feature and Blueprint Queries
- `get_feature_provider()`: Finds components providing specific features
- `get_blueprint_provider()`: Locates components providing specific blueprints

## Implementation Details

### Dirty State Tracking
- Maintains `database_is_dirty` flag
- Automatic saving on destruction if dirty
- Manual save operations possible

### Error Handling
- Uses `std::expected` for operation results
- Provides detailed error codes and messages
- Graceful handling of missing files and permissions

### File System Integration
- Uses `std::filesystem` for path operations
- Supports both relative and absolute paths
- Handles path normalization and validation

### Component Flags
Supports different component loading modes:
- `IGNORE_ALL_SLC`: Excludes all SLC format files
- `IGNORE_YAKKA`: Excludes Yakka format files
- `ONLY_SLCC`: Restricts to SLCC format only

## Usage Example

```cpp
// Create and initialize database
yakka::component_database db;
db.load(workspace_path);

// Scan for components
db.scan_for_components();

// Query component information
auto component = db.get_component("my-component");
if(component) {
    // Process component
}

// Find feature providers
auto provider = db.get_feature_provider("feature-name");
if(provider) {
    // Use provider information
}
```

## Performance Considerations

- Lazy loading of component contents
- Cached database file for fast startup
- Efficient component lookup through JSON structure
- Minimal disk I/O through dirty state tracking

## Integration Points

- Project configuration system
- Build system
- Feature resolution system
- Blueprint execution engine
- Workspace management

## Future Considerations

- Component versioning support
- Improved caching mechanisms
- Extended metadata support
- Enhanced validation capabilities
- Remote component registry support
