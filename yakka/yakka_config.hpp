#pragma once

#include "yakka_workspace.hpp"
#include <httplib.h>
#include <string>

namespace yakka {

// Start the configuration server on localhost:8080
void start_config_server(yakka::workspace& workspace, bool& server_running);

// Helper functions for logging
std::string dump_headers(const httplib::Headers& headers);
std::string dump_request_response(const httplib::Request& req, const httplib::Response& res);

} // namespace yakka
