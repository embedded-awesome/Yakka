#include "yakka.hpp"
#include "yakka_config.hpp"
#include "yakka_component.hpp"
#include "yakka_workspace.hpp"
#include "spdlog.h"
#include <httplib.h>
#include <chrono>

namespace yakka {

void start_config_server(yakka::workspace& workspace, bool& server_running) {
  httplib::Server server;

  if (!server.is_valid()) {
    spdlog::error("Server has an error...\n");
    return;
  }

  server.Options("/(.*)", [&](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Methods", "*");
    res.set_header("Access-Control-Allow-Headers", "*");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Connection", "close");
  });

//   workspace.init(".");

  server.Get("/api/components", [&](const httplib::Request& req, httplib::Response& res) {
    spdlog::info("GET /api/components\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.local_database.dump(), "application/json");
  });

  server.Get("/api/projects", [&](const httplib::Request& req, httplib::Response& res) {
    spdlog::info("GET /api/projects\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.projects.dump(), "application/json");
  });

  server.Get("/api/component/:id", [&](const httplib::Request& req, httplib::Response& res) {
    spdlog::info("GET /api/component/{}\n", req.path_params.at("id"));
    res.set_header("Access-Control-Allow-Origin", "*");
    auto component = req.path_params.at("id");
    auto component_path = workspace.local_database.get_component(component);
    if (component_path.has_value()) {
      yakka::component component_data;
      component_data.parse_file(component_path.value());
      res.set_content(component_data.json.dump(2), "application/json");
    } else {
      res.status = 404;
    }
  });

  server.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
    const char* fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
    char buf[BUFSIZ];
    snprintf(buf, sizeof(buf), fmt, res.status);
    res.set_content(buf, "text/html");
  });

  server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
    spdlog::debug(dump_request_response(req, res));
  });

  spdlog::set_level(spdlog::level::info);
  spdlog::info("Server is running on http://localhost:8080");
  server_running = true;
  server.listen("127.0.0.1", 8080);
  server_running = false;
}

std::string dump_headers(const httplib::Headers& headers) {
  std::string s;
  char buf[BUFSIZ];

  for (auto it = headers.begin(); it != headers.end(); ++it) {
    const auto& x = *it;
    snprintf(buf, sizeof(buf), "%s: %s\n", x.first.c_str(), x.second.c_str());
    s += buf;
  }

  return s;
}

std::string dump_request_response(const httplib::Request& req, const httplib::Response& res) {
  std::string s;
  char buf[BUFSIZ];

  s += "================================\n";

  snprintf(buf, sizeof(buf), "%s %s %s", req.method.c_str(), req.version.c_str(), req.path.c_str());
  s += buf;

  std::string query;
  for (auto it = req.params.begin(); it != req.params.end(); ++it) {
    const auto& x = *it;
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

} // namespace yakka
