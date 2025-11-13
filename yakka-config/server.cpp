#include "httplib.h"
#include "spdlog.h"
#include "yakka_component.hpp"
#include "yakka_workspace.hpp"
#include <chrono>
#include <cstdio>

using namespace httplib;

static std::string log(const Request &req, const Response &res);
static std::string dump_headers(const Headers &headers);

int main(void)
{
  Server server;
  yakka::workspace workspace;

  if (!server.is_valid()) {
    spdlog::error("Server has an error...\n");
    return -1;
  }

  server.Options("/(.*)", [&](const Request & /*req*/, Response &res) {
    res.set_header("Access-Control-Allow-Methods", "*");
    res.set_header("Access-Control-Allow-Headers", "*");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Connection", "close");
  });

  workspace.init(".");

  server.Get("/api/components", [&](const Request &req, Response &res) {
    spdlog::info("GET /api/components\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.local_database.dump(), "application/json");
  });

  server.Get("/api/projects", [&](const Request &req, Response &res) {
    spdlog::info("GET /api/projects\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.projects.dump(), "application/json");
  });

  server.Get("/api/component/:id", [&](const Request &req, Response &res) {
    spdlog::info("GET /api/component/{}\n", req.path_params.at("id"));
    res.set_header("Access-Control-Allow-Origin", "*");
    auto component      = req.path_params.at("id");
    auto component_path = workspace.local_database.get_component(component);
    if (component_path.has_value()) {
      yakka::component component_data;
      component_data.parse_file(component_path.value());
      res.set_content(component_data.json.dump(2), "application/json");
    } else {
      res.status = 404;
    }
  });

  server.set_error_handler([](const Request & /*req*/, Response &res) {
    const char *fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
    char buf[BUFSIZ];
    snprintf(buf, sizeof(buf), fmt, res.status);
    res.set_content(buf, "text/html");
  });

  server.set_logger([](const Request &req, const Response &res) {
    spdlog::debug(log(req, res));
  });

  spdlog::info("Server is running on http://localhost:8080\n");
  server.listen("127.0.0.1", 8080);

  return 0;
}

static std::string dump_headers(const Headers &headers)
{
  std::string s;
  char buf[BUFSIZ];

  for (auto it = headers.begin(); it != headers.end(); ++it) {
    const auto &x = *it;
    snprintf(buf, sizeof(buf), "%s: %s\n", x.first.c_str(), x.second.c_str());
    s += buf;
  }

  return s;
}

static std::string log(const Request &req, const Response &res)
{
  std::string s;
  char buf[BUFSIZ];

  s += "================================\n";

  snprintf(buf, sizeof(buf), "%s %s %s", req.method.c_str(), req.version.c_str(), req.path.c_str());
  s += buf;

  std::string query;
  for (auto it = req.params.begin(); it != req.params.end(); ++it) {
    const auto &x = *it;
    snprintf(buf, sizeof(buf), "%c%s=%s", (it == req.params.begin()) ? '?' : '&', x.first.c_str(), x.second.c_str());
    query += buf;
  }
  snprintf(buf, sizeof(buf), "%s\n", query.c_str());
  s += buf;

  s += dump_headers(req.headers);

  s += "--------------------------------\n";

  snprintf(buf, sizeof(buf), "%d %s\n", res.status, res.version.c_str());
  s += buf;
  s += dump_headers(res.headers);
  s += "\n";

  if (!res.body.empty()) {
    s += res.body;
  }

  s += "\n";

  return s;
}