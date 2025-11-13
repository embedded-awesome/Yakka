// FILEPATH: /C:/silabs/yakka/yakka/yakka_project_test.cpp

#include "gtest/gtest.h"
#include "yakka_project.hpp"

class YakkaProjectTest : public ::testing::Test {
protected:
  yakka::project project;
};

TEST_F(YakkaProjectTest, TestInitProject)
{
  EXPECT_NO_THROW(project.init_project("test project"));
}

TEST_F(YakkaProjectTest, TestEvaluateDependencies)
{
  EXPECT_NO_THROW(project.evaluate_dependencies());
}

TEST_F(YakkaProjectTest, TestGenerateProjectSummary)
{
  EXPECT_NO_THROW(project.generate_project_summary());
}

TEST_F(YakkaProjectTest, TestSaveSummary)
{
  EXPECT_NO_THROW(project.save_summary());
}

TEST_F(YakkaProjectTest, TestParseBlueprints)
{
  EXPECT_NO_THROW(project.parse_blueprints());
}

TEST_F(YakkaProjectTest, TestGenerateTargetDatabase)
{
  EXPECT_NO_THROW(project.generate_target_database());
}

TEST_F(YakkaProjectTest, TestLoadCommonCommands)
{
  EXPECT_NO_THROW(project.load_common_commands());
}

/*
*/

// FILEPATH: /C:/silabs/yakka/tests/yakka_component_fuzz_test.cpp

#include "gtest/gtest.h"
#include "yakka_component.hpp"
#include <fuzzer/FuzzedDataProvider.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  FuzzedDataProvider fuzzed_data(data, size);

  while (fuzzed_data.remaining_bytes() > 0) {
    yakka::component component;

    try {
      std::string fuzzed_string = fuzzed_data.ConsumeRandomLengthString();
      component.parse_component(fuzzed_string);
    } catch (...) {
      // Ignore exceptions, we're just fuzzing
    }
  }

  return 0; // Non-zero return values are reserved for future use.
}

TEST(YakkaComponentFuzzTest, TestParseComponent)
{
  // This is a dummy test case to satisfy Google Test's requirement
  // that we have at least one TEST or TEST_F in this file.
  EXPECT_TRUE(true);
}