#include "utilities.hpp"
#include <gtest/gtest.h>

namespace {

class DataDependencyTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    left_tree = ryml::parse_in_arena(R"({
      "components": {
        "comp1": {"data": {"value": 42, "name": "test"}},
        "comp2": {"data": {"value": 100, "name": "other"}}
      },
      "data": {"value": 7}
    })");
    right_tree = ryml::parse_in_arena(R"({
      "components": {
        "comp1": {"data": {"value": 42, "name": "test"}},
        "comp2": {"data": {"value": 100, "name": "other"}}
      },
      "data": {"value": 7}
    })");
  }

  ryml::Tree left_tree;
  ryml::Tree right_tree;
};

TEST_F(DataDependencyTest, NonDependencyPathIsUnchanged)
{
  auto result = yakka::has_data_dependency_changed("data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
}

TEST_F(DataDependencyTest, RejectsMalformedDependencyPath)
{
  auto result = yakka::has_data_dependency_changed(":/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);

  result = yakka::has_data_dependency_changed(":invalid", left_tree.crootref(), right_tree.crootref());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("Invalid path format"), std::string::npos);

  result = yakka::has_data_dependency_changed(":", left_tree.crootref(), right_tree.crootref());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("Invalid path format"), std::string::npos);
}

TEST_F(DataDependencyTest, DetectsChangedComponentValue)
{
  right_tree = ryml::parse_in_arena(R"({
    "components": {
      "comp1": {"data": {"value": 43, "name": "test"}},
      "comp2": {"data": {"value": 100, "name": "other"}}
    },
    "data": {"value": 7}
  })");

  auto result = yakka::has_data_dependency_changed(":/components/comp1/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(DataDependencyTest, EqualComponentValueIsUnchanged)
{
  auto result = yakka::has_data_dependency_changed(":/components/comp1/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
}

TEST_F(DataDependencyTest, DetectsChangesToAnyWildcardComponent)
{
  right_tree = ryml::parse_in_arena(R"({
    "components": {
      "comp1": {"data": {"value": 42, "name": "test"}},
      "comp2": {"data": {"value": 101, "name": "other"}}
    },
    "data": {"value": 7}
  })");

  auto result = yakka::has_data_dependency_changed(":/components/*/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(DataDependencyTest, DetectsAddedWildcardComponents)
{
  right_tree = ryml::parse_in_arena(R"({
    "components": {
      "comp1": {"data": {"value": 42, "name": "test"}},
      "comp2": {"data": {"value": 100, "name": "other"}},
      "comp3": {"data": {"value": 200, "name": "new"}}
    },
    "data": {"value": 7}
  })");

  auto result = yakka::has_data_dependency_changed(":/components/*/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(DataDependencyTest, DetectsRemovedWildcardComponents)
{
  right_tree = ryml::parse_in_arena(R"({
    "components": {
      "comp1": {"data": {"value": 42, "name": "test"}}
    },
    "data": {"value": 7}
  })");

  auto result = yakka::has_data_dependency_changed(":/components/*/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(DataDependencyTest, DetectsChangedRootData)
{
  right_tree = ryml::parse_in_arena(R"({
    "components": {
      "comp1": {"data": {"value": 42, "name": "test"}},
      "comp2": {"data": {"value": 100, "name": "other"}}
    },
    "data": {"value": 8}
  })");

  auto result = yakka::has_data_dependency_changed(":/data/value", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

TEST_F(DataDependencyTest, MissingPathOnBothSidesIsUnchanged)
{
  auto result = yakka::has_data_dependency_changed(":/components/comp1/data/missing", left_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(*result);
}

TEST_F(DataDependencyTest, MissingComponentDataIsChanged)
{
  ryml::Tree empty_tree;
  empty_tree.rootref() |= ryml::MAP;

  auto result = yakka::has_data_dependency_changed(":/components/comp1/data/value", empty_tree.crootref(), right_tree.crootref());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result);
}

} // namespace
