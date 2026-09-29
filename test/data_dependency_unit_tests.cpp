#include "utilities.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
    // Test fixture for yakka::has_data_dependency_changed tests
    class DataDependencyTest : public ::testing::Test {
    protected:
        void SetUp() override {
            // Sample JSON data structure
            base_json = R"({
                "components": {
                    "comp1": {
                        "data": {
                            "value": 42,
                            "name": "test"
                        }
                    },
                    "comp2": {
                        "data": {
                            "value": 100,
                            "name": "other"
                        }
                    }
                }
            })"_json;

            // Constants used in the original function
            data_dependency_identifier = ':';
            data_wildcard_identifier = '*';
        }

        nlohmann::json base_json;
        char data_dependency_identifier;
        char data_wildcard_identifier;
    };

    TEST_F(DataDependencyTest, InvalidPathPrefix) {
        auto result = yakka::has_data_dependency_changed("invalid/path", base_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }

    TEST_F(DataDependencyTest, MalformedPath) {
        auto result = yakka::has_data_dependency_changed(":invalid", base_json, base_json);
        ASSERT_FALSE(result.has_value());
        EXPECT_TRUE(result.error().find("Invalid path format") != std::string::npos);
    }

    TEST_F(DataDependencyTest, NullLeftJson) {
        nlohmann::json null_json;
        auto result = yakka::has_data_dependency_changed(":/comp1/data/value", null_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, SingleComponentNoChange) {
        auto modified_json = base_json;
        auto result = yakka::has_data_dependency_changed(":/comp1/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }

    TEST_F(DataDependencyTest, SingleComponentWithChange) {
        auto modified_json = base_json;
        modified_json["components"]["comp1"]["data"]["value"] = 43;
        auto result = yakka::has_data_dependency_changed(":/comp1/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, NonexistentComponent) {
        auto result = yakka::has_data_dependency_changed(":/comp3/data/value", base_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, WildcardNoChanges) {
        auto result = yakka::has_data_dependency_changed(":/*/data/value", base_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }

    TEST_F(DataDependencyTest, WildcardWithChanges) {
        auto modified_json = base_json;
        modified_json["components"]["comp2"]["data"]["value"] = 101;
        auto result = yakka::has_data_dependency_changed(":/*/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, WildcardMalformedPath) {
        auto result = yakka::has_data_dependency_changed(":/*data", base_json, base_json);
        ASSERT_FALSE(result.has_value());
        EXPECT_TRUE(result.error().find("Data dependency malformed") != std::string::npos);
    }

    TEST_F(DataDependencyTest, NonexistentPath) {
        auto result = yakka::has_data_dependency_changed(":/comp1/nonexistent/path", base_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }

    TEST_F(DataDependencyTest, AddedComponent) {
        auto modified_json = base_json;
        modified_json["components"]["comp3"] = {{"data", {{"value", 200}}}};
        auto result = yakka::has_data_dependency_changed(":/*/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, RemovedComponent) {
        auto modified_json = base_json;
        modified_json["components"].erase("comp2");
        auto result = yakka::has_data_dependency_changed(":/comp2/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, ComponentTypeChange) {
        auto modified_json = base_json;
        modified_json["components"]["comp1"]["data"]["value"] = "42";  // Changed from int to string
        auto result = yakka::has_data_dependency_changed(":/comp1/data/value", base_json, modified_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }

    TEST_F(DataDependencyTest, EmptyPath) {
        auto result = yakka::has_data_dependency_changed("", base_json, base_json);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }

    TEST(RymlDiffTest, GeneratesNestedStructureOnDifference) {
        auto left_tree = ryml::parse_in_arena(R"({
            "components": {
                "comp1": {
                    "version": "1.0",
                    "sources": ["a.cpp"]
                },
                "comp2": {
                    "version": "2.0"
                }
            }
        })");
        auto right_tree = ryml::parse_in_arena(R"({
            "components": {
                "comp1": {
                    "version": "0.9",
                    "sources": ["a.cpp"]
                }
            }
        })");

        ryml::Tree diff_tree;
        auto diff_root = diff_tree.rootref();
        diff_root.set_map();

        yakka::ryml_diff(left_tree.crootref(), right_tree.crootref(), diff_root);

        ASSERT_TRUE(diff_root.is_map());
        ASSERT_TRUE(diff_root.has_child("components"));
        auto components = diff_root["components"];
        ASSERT_TRUE(components.has_child("comp1"));
        EXPECT_EQ(components["comp1"]["version"].val(), c4::to_csubstr("1.0"));
        EXPECT_FALSE(components["comp1"].has_child("sources"));
        ASSERT_TRUE(components.has_child("comp2"));
        EXPECT_EQ(components["comp2"]["version"].val(), c4::to_csubstr("2.0"));
    }

    TEST(RymlDiffTest, PreservesSequenceIndexWithPlaceholders) {
        auto left_tree  = ryml::parse_in_arena(R"({"process": [{"echo": "one"}, {"echo": "two"}, {"echo": "Done yay"}]})");
        auto right_tree = ryml::parse_in_arena(R"({"process": [{"echo": "one"}, {"echo": "two"}, {"echo": "nope"}]})");

        ryml::Tree diff_tree;
        auto diff_root = diff_tree.rootref();
        diff_root.set_map();

        yakka::ryml_diff(left_tree.crootref(), right_tree.crootref(), diff_root);

        ASSERT_TRUE(diff_root.has_child("process"));
        auto process = diff_root["process"];
        ASSERT_TRUE(process.is_seq());
        ASSERT_EQ(process.num_children(), 3u);
        EXPECT_EQ(process[0].val(), c4::to_csubstr("null"));
        EXPECT_EQ(process[1].val(), c4::to_csubstr("null"));
        EXPECT_EQ(process[2]["echo"].val(), c4::to_csubstr("Done yay"));
    }
}