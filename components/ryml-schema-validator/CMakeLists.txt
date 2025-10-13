cmake_minimum_required(VERSION 3.14)
project(ryml-schema-validator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find rapidyaml
find_package(rapidyaml REQUIRED)

# Create library
add_library(ryml-schema
    src/validator.cpp
    src/string-schema.cpp
    src/number-schema.cpp
    src/object-schema.cpp
    src/array-schema.cpp
    src/ryml-schema-combinators.cpp
)

target_include_directories(ryml-schema
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ryml-schema
    PUBLIC
        rapidyaml::rapidyaml
)