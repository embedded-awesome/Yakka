#include "yakka.hpp"
#include "yakka_config.hpp"
#include "yakka_component.hpp"
#include "yakka_workspace.hpp"
#include "yakka_project.hpp"
#include "utilities.hpp"
#include "spdlog.h"
#include <ryml.hpp>
#include <ryml_std.hpp> // For std::string and std::vector interop
#include <httplib.h>
#include <chrono>

namespace yakka {

static std::string extract_start_of_url_path(const std::string &url_path)
{
  if (url_path.empty() || url_path[0] != '/')
    return "";

  size_t start = 1;
  size_t end   = url_path.find('/', start);
  return url_path.substr(start, end - start);
}

void start_config_server(yakka::workspace &workspace, bool &server_running)
{
  httplib::Server server;

  if (!server.is_valid()) {
    spdlog::error("Server has an error...\n");
    return;
  }

  // Load the component registries
  workspace.load_component_registries();

  server.Options("/(.*)", [&](const httplib::Request & /*req*/, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Methods", "*");
    res.set_header("Access-Control-Allow-Headers", "*");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Connection", "close");
  });

  server.set_pre_routing_handler([&](const auto &req, auto &res) {
    const auto start = extract_start_of_url_path(req.path);
    if (start == "api" || start == "assets") {
      return httplib::Server::HandlerResponse::Unhandled;
    }

    // Else try to find a serve endpoint
    auto endpoint = workspace.local_database.get_serve_endpoint_provider(start);
    if (endpoint.has_value()) {
      // Find details of the component providing this endpoint
      auto component_name = endpoint.value()[0].template get<std::string>();
      auto component_path = workspace.local_database.get_component(component_name);
      if (component_path.has_value()) {
        // Check if the request is for an endpoint or a file
        auto full_path                 = (*component_path).parent_path();
        std::filesystem::path req_path = req.path.substr(1); // 1 to skip the leading '/'
        if (req_path.extension() != "") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        full_path /= "index.html";
      
        spdlog::get("console")->info("Serving {} for endpoint {}", full_path.string(), req.path);
        if (fs::exists(full_path)) {
          std::ifstream file(full_path);
          std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
          res.set_content(content, "text/html");
          file.close();
          return httplib::Server::HandlerResponse::Handled;
        }
      }
    }
    res.status = 404;
    return httplib::Server::HandlerResponse::Handled;
  });

  // server.set_mount_point("/assets", "./components/configurator/assets");
  // Loop through the 'serve' endpoints in the local database and mount them
  for (const auto &i: workspace.local_database.database["serve"].items()) {
    const auto endpoint     = i.key();
    const auto component_id = i.value()[0].template get<std::string>();
    auto component_path     = workspace.local_database.get_component(component_id);
    if (!component_path.has_value())
      continue;

    auto serve_path = (*component_path).parent_path();
    spdlog::info("Mounting endpoint /{} to {}\n", endpoint, serve_path.string());
    server.set_mount_point("/" + endpoint, serve_path.string());
  }

  server.Get("/api/components", [&](const httplib::Request &req, httplib::Response &res) {
    spdlog::info("GET /api/components\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.local_database.dump(), "application/json");
  });

  server.Get("/api/projects", [&](const httplib::Request &req, httplib::Response &res) {
    spdlog::info("GET /api/projects\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.projects.dump(), "application/json");
  });

  server.Get("/api/registries", [&](const httplib::Request &req, httplib::Response &res) {
    spdlog::info("GET /api/registries\n");
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_content(workspace.registries.as<nlohmann::json>().dump(), "application/json");
  });

  server.Get("/api/project/:id", [&](const httplib::Request &req, httplib::Response &res) {
    spdlog::info("GET /api/project/{}\n", req.path_params.at("id"));
    res.set_header("Access-Control-Allow-Origin", "*");
    auto project = req.path_params.at("id");
    if (!workspace.projects.contains(project)) {
      res.status = 404;
      return;
    }
    auto project_summary_file = std::filesystem::path{ workspace.projects[project]["path"].get<std::string>() } / project_summary_filename;
    if (fs::exists(project_summary_file)) {
      // Read the entire file content into json object
      std::ifstream project_summary_stream(project_summary_file);
      nlohmann::json project_summary = nlohmann::json::parse(project_summary_stream);
      const auto project_file        = project_summary["project_name"].get<std::string>() + ".yakka";
      if (fs::exists(project_file)) {
        YAML::Node node = YAML::LoadFile(project_file);
        // Merge data from the project file
        json_node_merge("/data"_json_pointer, project_summary, node.as<nlohmann::json>());
      }

      // std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      res.set_content(project_summary.dump(), "application/json"); // Set appropriate Content-Type
    } else {
      res.status = 404;
      return;
    }
  });

  server.Post("/api/project/:id/data", [&](const httplib::Request &req, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    spdlog::get("console")->info("POST /api/project/{}/data\n", req.path_params.at("id"));
    auto project_id = req.path_params.at("id");

    // Load up project summary and see if the project file exists
    yakka::project the_project(project_id, workspace);
    the_project.init_project();

    // If not, create a new project file
    if (the_project.project_file.empty() || !fs::exists(the_project.project_file)) {
      the_project.create_project_file();
    }

    // Now parse the incoming JSON data and update the project file
    try {
      ryml::Tree incoming_data = ryml::parse_in_arena(ryml::to_csubstr(req.body));

      std::fstream project_file_stream(the_project.project_file, std::ios::in | std::ios::out);
      std::stringstream buffer;
      buffer << project_file_stream.rdbuf();
      std::string yaml_str    = buffer.str();
      ryml::Tree project_data = ryml::parse_in_arena(ryml::to_csubstr(yaml_str));

      project_data.merge_with(&incoming_data);

      project_file_stream.seekp(0);
      project_file_stream << project_data;
      project_file_stream.close();
      res.set_content("{\"status\": \"OK\"}", "application/json");
    } catch (const nlohmann::json::parse_error &e) {
      spdlog::error("Failed to parse incoming JSON data: {}\n", e.what());
      res.status = 400;
      return;
    }
  });

  server.Get("/api/component/:id", [&](const httplib::Request &req, httplib::Response &res) {
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

  server.set_error_handler([](const httplib::Request & /*req*/, httplib::Response &res) {
    const char *fmt = "<p>Error Status: <span style='color:red;'>%d</span></p>";
    char buf[BUFSIZ];
    snprintf(buf, sizeof(buf), fmt, res.status);
    res.set_content(buf, "text/html");
  });

  server.set_logger([](const httplib::Request &req, const httplib::Response &res) {
    spdlog::debug(dump_request_response(req, res));
  });

  spdlog::get("console")->info("Server is running on http://localhost:8080");
  server_running = true;
  server.listen("127.0.0.1", 8080);
  server_running = false;
}

std::string dump_headers(const httplib::Headers &headers)
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

std::string dump_request_response(const httplib::Request &req, const httplib::Response &res)
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

} // namespace yakka
