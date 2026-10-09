#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

using task_graph = std::map<std::string, std::set<std::string>>;

task_graph describe(const tf::Taskflow &taskflow)
{
  task_graph result;
  taskflow.for_each_task([&](tf::Task task) {
    auto &successors = result[task.name()];
    task.for_each_successor([&](tf::Task successor) {
      successors.insert(successor.name());
    });
  });
  return result;
}

class TaskEnginePersistenceTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    filename = fs::temp_directory_path() / ("yakka_taskflow_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
  }

  void TearDown() override
  {
    fs::remove(filename);
  }

  fs::path filename;
};

TEST_F(TaskEnginePersistenceTest, RoundTripsNamesAndDependencies)
{
  yakka::task_engine source;
  auto compile = source.taskflow.placeholder().name("compile");
  auto archive = source.taskflow.placeholder().name("archive");
  auto link    = source.taskflow.placeholder().name("link");
  compile.precede(archive, link);
  archive.precede(link);

  source.save_taskflow(filename);

  yakka::task_engine loaded;
  loaded.load_taskflow(filename);

  EXPECT_EQ(describe(loaded.taskflow), describe(source.taskflow));
  EXPECT_TRUE(loaded.is_valid());
}

TEST_F(TaskEnginePersistenceTest, SaveTaskflowIncludesPredecessors)
{
  yakka::task_engine source;
  auto compile = source.taskflow.placeholder().name("compile");
  auto archive = source.taskflow.placeholder().name("archive");
  auto link    = source.taskflow.placeholder().name("link");
  compile.precede(archive, link);
  archive.precede(link);

  source.save_taskflow(filename);

  std::ifstream input(filename);
  std::stringstream buffer;
  buffer << input.rdbuf();

  const std::string json = buffer.str();
  EXPECT_NE(json.find("\"predecessors\""), std::string::npos);
  EXPECT_NE(json.find("\"successors\""), std::string::npos);

  EXPECT_NE(json.find("\"name\": \"compile\""), std::string::npos);
  EXPECT_NE(json.find("\"name\": \"archive\""), std::string::npos);
  EXPECT_NE(json.find("\"name\": \"link\""), std::string::npos);

  EXPECT_NE(json.find("\"name\": \"archive\",\"predecessors\": [0]"), std::string::npos);
  EXPECT_NE(json.find("\"name\": \"link\",\"predecessors\": [0,1]"), std::string::npos);
}

TEST_F(TaskEnginePersistenceTest, InvalidSuccessorPreservesExistingGraph)
{
  std::ofstream file(filename);
  file << R"({"version":1,"tasks":[{"name":"invalid","successors":[1]}]})";
  file.close();

  yakka::task_engine engine;
  engine.taskflow.placeholder().name("existing");
  const auto before = describe(engine.taskflow);

  engine.load_taskflow(filename);

  EXPECT_EQ(describe(engine.taskflow), before);
}

TEST_F(TaskEnginePersistenceTest, TodoListRoundTripsConstructedTasks)
{
  yakka::task_engine source;
  auto compile = source.taskflow.placeholder().name("compile");
  auto archive = source.taskflow.placeholder().name("archive");
  compile.precede(archive);

  auto compile_entry = std::make_shared<yakka::construction_task>();
  compile_entry->task = compile;
  // An epoch timestamp marks the target as never built, which forces load_todolist to rebuild it
  compile_entry->last_modified = fs::file_time_type{};
  compile_entry->group = std::make_shared<yakka::task_group>("build");
  compile_entry->group->total_count = 2;
  auto archive_entry = std::make_shared<yakka::construction_task>();
  archive_entry->task = archive;
  source.todo_list.insert({"archive", archive_entry});
  source.todo_list.insert({"compile", compile_entry});
  source.todo_task_groups["build"] = compile_entry->group;

  source.save_todolist(filename);

  std::ifstream input(filename);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string json = buffer.str();
  EXPECT_NE(json.find("\"version\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"target\": \"compile\""), std::string::npos);
  EXPECT_NE(json.find("\"successors\": [0]"), std::string::npos);
  EXPECT_EQ(json.find("\"task_name\""), std::string::npos);
  EXPECT_EQ(json.find("\"task_hash\""), std::string::npos);
  EXPECT_EQ(json.find("\"group_name\""), std::string::npos);

  const auto unchanged_time = fs::file_time_type::clock::now() - std::chrono::minutes(1);
  fs::last_write_time(filename, unchanged_time);
  source.save_todolist(filename);
  EXPECT_EQ(fs::last_write_time(filename), unchanged_time);

  yakka::workspace workspace;
  yakka::project project(workspace, "todolist_round_trip");

  yakka::task_engine loaded;
  EXPECT_EQ(loaded.load_todolist(filename, project), yakka::yakka_status::SUCCESS);
  EXPECT_EQ(loaded.todo_list.count("compile"), 1u);
  EXPECT_EQ(loaded.todo_list.find("compile")->second->task.name(), "compile");
  EXPECT_EQ(loaded.todo_list.count("archive"), 1u);
  EXPECT_EQ(describe(loaded.taskflow), (task_graph{{"archive", {}}, {"compile", {"archive"}}}));
}

} // namespace