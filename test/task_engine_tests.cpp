#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"
#include "file_check.hpp"
#include <gtest/gtest.h>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"
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
    previous_logger = spdlog::default_logger();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(log_output);
    auto logger = std::make_shared<spdlog::logger>("task_engine_persistence_test", sink);
    logger->set_level(spdlog::level::warn);
    logger->set_pattern("%v");
    spdlog::set_default_logger(logger);
  }

  void TearDown() override
  {
    spdlog::set_default_logger(previous_logger);
    spdlog::drop("task_engine_persistence_test");
    fs::remove(filename);
  }

  void write_large_todolist(size_t count, size_t edges_per_item, bool dirty_first = false)
  {
    ryml::Tree document;
    auto root = document.rootref();
    root |= ryml::MAP;
    root["version"] << 2;
    root["file_checks"] << yakka::file_check_policy;
    root["successful"] << true;
    auto items = root["items"];
    items |= ryml::SEQ;
    for (size_t index = 0; index < count; ++index) {
      auto item = items.append_child();
      item |= ryml::MAP;
      item["target"] << filename.generic_string() + ".missing/" + std::to_string(index);
      item["last_modified"] << 1;
      item["has_match"] << 0;
      auto check = item["file_check"];
      check |= ryml::MAP;
      check["policy"] << (dirty_first && index == 0 ? "stale" : yakka::file_check_policy);
      check["exists"] << false;
      item["successors"] |= ryml::SEQ;
      if (index + 1 < count)
        for (size_t edge = 0; edge < edges_per_item; ++edge)
          item["successors"].append_child() << count - 1;
    }
    yakka::write_file_if_changed(filename, ryml::emitrs_json<std::string>(document));
  }

  fs::path filename;
  std::ostringstream log_output;
  std::shared_ptr<spdlog::logger> previous_logger;
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

TEST_F(TaskEnginePersistenceTest, TodoListExcludesInternalFinishTask)
{
  yakka::workspace workspace;
  yakka::project project(workspace, "todolist_finish");
  project.commands.insert("link");
  project.target_database.targets.try_emplace("link");

  yakka::task_engine source;
  source.create_taskflow(project);
  ASSERT_EQ(source.todo_list.count("link"), 1u);
  auto link = source.todo_list.find("link")->second->task;
  auto compile_entry = source.create_task_leaf_node("compile", project, fs::file_time_type::clock::now());
  compile_entry->last_modified = fs::file_time_type{};
  compile_entry->task.precede(link);
  const auto original_graph = describe(source.taskflow);
  EXPECT_EQ(original_graph, (task_graph{{"", {}}, {"compile", {"link"}}, {"link", {""}}}));

  source.save_todolist(filename);
  EXPECT_TRUE(log_output.str().empty()) << log_output.str();
  EXPECT_EQ(describe(source.taskflow), original_graph);

  yakka::task_engine loaded;
  ASSERT_EQ(loaded.load_todolist(filename, project), yakka::yakka_status::SUCCESS);
  EXPECT_EQ(loaded.todo_list.size(), 2u);
  EXPECT_EQ(describe(loaded.taskflow), (task_graph{{"compile", {"link"}}, {"link", {}}}));

  source.save_taskflow(filename);
  yakka::task_engine full_graph;
  ASSERT_EQ(full_graph.load_taskflow(filename), yakka::yakka_status::SUCCESS);
  EXPECT_EQ(describe(full_graph.taskflow), original_graph);
}

TEST_F(TaskEnginePersistenceTest, TodoListStillWarnsForMissingSuccessors)
{
  yakka::workspace workspace;
  yakka::project project(workspace, "todolist_missing");
  project.commands.insert("link");
  project.target_database.targets.try_emplace("link");

  yakka::task_engine source;
  source.create_taskflow(project);
  ASSERT_EQ(source.todo_list.count("link"), 1u);
  auto link = source.todo_list.find("link")->second->task;
  auto missing_named = source.taskflow.placeholder().name("missing");
  auto missing_unnamed = source.taskflow.placeholder();
  link.precede(missing_named, missing_unnamed);

  source.save_todolist(filename);

  EXPECT_EQ(log_output.str(),
            "Todo list successor 'missing' of 'link' is missing while saving\n"
            "Todo list successor '' of 'link' is missing while saving\n");
}

TEST_F(TaskEnginePersistenceTest, ReplacingTaskflowClearsInternalFinishIdentity)
{
  yakka::workspace workspace;
  yakka::project project(workspace, "todolist_replaced");
  project.commands.insert("link");
  project.target_database.targets.try_emplace("link");

  yakka::task_engine source;
  source.create_taskflow(project);
  std::ofstream file(filename);
  file << R"({"version":1,"tasks":[{"name":"link","successors":[1]},{"name":"","successors":[]}]})";
  file.close();
  ASSERT_EQ(source.load_taskflow(filename), yakka::yakka_status::SUCCESS);

  auto link_entry = std::make_shared<yakka::construction_task>();
  source.taskflow.for_each_task([&](tf::Task task) {
    if (task.name() == "link")
      link_entry->task = task;
  });
  source.todo_list.insert({"link", link_entry});
  source.save_todolist(filename);

  EXPECT_EQ(log_output.str(), "Todo list successor '' of 'link' is missing while saving\n");
}

TEST_F(TaskEnginePersistenceTest, LargeUnchangedTodolistScansWithoutPerEdgeTreeWalks)
{
  write_large_todolist(6000, 14);
  yakka::workspace workspace;
  yakka::project project(workspace, "large_unchanged");
  yakka::task_engine engine;
  const auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(engine.load_todolist(filename, project), yakka::yakka_status::SUCCESS);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  RecordProperty("todolist_scan_ms", std::chrono::duration<double, std::milli>(elapsed).count());
  EXPECT_LT(elapsed, std::chrono::seconds(1));
  EXPECT_TRUE(engine.todo_list.empty());
  EXPECT_EQ(engine.taskflow.num_tasks(), 0u);
  EXPECT_EQ(engine.todo_list_tree.crootref()["items"].num_children(), 6000u);
}

TEST_F(TaskEnginePersistenceTest, LargeTodolistRestoresHighIndexSuccessorsAndCarriesOtherEntries)
{
  write_large_todolist(3000, 14, true);
  yakka::workspace workspace;
  yakka::project project(workspace, "large_changed");
  yakka::task_engine engine;
  ASSERT_EQ(engine.load_todolist(filename, project), yakka::yakka_status::SUCCESS);
  ASSERT_EQ(engine.todo_list.size(), 2u);
  const auto prefix = filename.generic_string() + ".missing/";
  ASSERT_EQ(engine.todo_list.count(c4::to_csubstr(prefix + "0")), 1u);
  ASSERT_EQ(engine.todo_list.count(c4::to_csubstr(prefix + "2999")), 1u);
  EXPECT_EQ(describe(engine.taskflow), (task_graph{ { prefix + "0", { prefix + "2999" } }, { prefix + "2999", {} } }));
  engine.save_todolist(filename);

  const auto saved = yakka::ryml_load_file(filename);
  ASSERT_TRUE(saved);
  std::vector<ryml::ConstNodeRef> items;
  for (const auto item: saved->crootref()["items"].children())
    items.push_back(item);
  ASSERT_EQ(items.size(), 3000u);
  size_t edges = 0;
  for (const auto item: items)
    for (const auto successor: item["successors"].children()) {
      const auto index = successor.val<size_t>();
      ASSERT_TRUE(index);
      ASSERT_LT(*index, items.size());
      EXPECT_EQ(items[*index]["target"].val(), c4::to_csubstr(prefix + "2999"));
      ++edges;
    }
  EXPECT_EQ(edges, 2999u * 14u);
}

TEST_F(TaskEnginePersistenceTest, RejectsOutOfRangeTodolistSuccessorBeforeRestoringTasks)
{
  write_large_todolist(3, 1, true);
  auto document = yakka::ryml_load_file(filename);
  ASSERT_TRUE(document);
  document->rootref()["items"][0]["successors"][0] << 3;
  yakka::write_file_if_changed(filename, ryml::emitrs_json<std::string>(*document));
  yakka::workspace workspace;
  yakka::project project(workspace, "invalid_successor");
  yakka::task_engine engine;
  EXPECT_EQ(engine.load_todolist(filename, project), yakka::yakka_status::FAIL);
  EXPECT_TRUE(engine.todo_list.empty());
  EXPECT_EQ(engine.taskflow.num_tasks(), 0u);
  EXPECT_NE(log_output.str().find("has an invalid successor of"), std::string::npos);
}

} // namespace