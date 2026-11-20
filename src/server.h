#pragma once

#include <atomic>
#include <string>

class Session;

struct ServerConfig {
  std::string host = "127.0.0.1";
  int port = 8099;
  std::string webRoot = "web";
};

bool runServer(Session& session, const ServerConfig& cfg, std::string& err,
               const std::atomic<bool>* stopFlag = nullptr);
