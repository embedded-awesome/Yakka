#include "yakka_workspace.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace {

class WorkspaceTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    test_dir = fs::temp_directory_path() / ("yakka_workspace_test_" +
                                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(test_dir);

    shared_home = test_dir / "shared";
    std::ofstream config_file(test_dir / "config.yaml");
    config_file << "home: " << shared_home.generic_string() << '\n';
    ASSERT_TRUE(config_file.good());

    workspace = std::make_unique<yakka::workspace>();
  }

  void TearDown() override
  {
    fs::remove_all(test_dir);
  }

  fs::path test_dir;
  fs::path shared_home;
  std::unique_ptr<yakka::workspace> workspace;
};

TEST_F(WorkspaceTest, InitializesWorkspaceDirectories)
{
  auto result = workspace->init(test_dir);

  ASSERT_TRUE(result);
  EXPECT_TRUE(fs::is_directory(test_dir / ".yakka/registries"));
  EXPECT_TRUE(fs::is_directory(test_dir / ".yakka/repos"));
  EXPECT_TRUE(fs::is_directory(shared_home));
  EXPECT_EQ(workspace->shared_components_path, shared_home);
}

TEST_F(WorkspaceTest, FindsNoComponentWhenDatabaseDoesNotContainIt)
{
  ASSERT_TRUE(workspace->init(test_dir));

  auto result = workspace->find_component(c4::to_csubstr("missing.component"));

  EXPECT_FALSE(result.has_value());
}

TEST_F(WorkspaceTest, FindsNoFeatureWhenNoProviderIsRegistered)
{
  ASSERT_TRUE(workspace->init(test_dir));

  EXPECT_FALSE(workspace->find_feature(c4::to_csubstr("missing.feature")).has_value());
}

TEST_F(WorkspaceTest, LoadsRegistriesAndFindsProvidedComponent)
{
  ASSERT_TRUE(workspace->init(test_dir));

  const auto registry_path = test_dir / ".yakka/registries/test.yaml";
  std::ofstream registry_file(registry_path);
  registry_file << R"(
provides:
  components:
    test.component:
      version: 1.0.0
)";
  registry_file.close();
  ASSERT_TRUE(registry_file.good());

  workspace->load_component_registries();
  auto component = workspace->find_registry_component(c4::to_csubstr("test.component"));

  ASSERT_TRUE(component.has_value());
  ASSERT_TRUE(component->has_child("version"));
  EXPECT_EQ(component->operator[]("version").val<std::string>().value(), "1.0.0");
}

TEST_F(WorkspaceTest, MissingConfigurationIsOptional)
{
  auto result = workspace->load_config_file(test_dir / "missing.yaml");

  EXPECT_TRUE(result.has_value());
}

} // namespace
