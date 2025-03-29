#pragma once

#include "esphome.h"
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESP32FtpServer.h>
#include <vector>
#include <string>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  
  void set_ftp_server(const std::string &server) { ftp_server_ = server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void add_remote_path(const std::string &path) { remote_paths_.push_back(path); }
  void set_local_port(uint16_t port) { local_port_ = port; }

 protected:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  std::vector<std::string> remote_paths_;
  uint16_t local_port_{8000};
  
  WebServer *server_{nullptr};
  WiFiClient ftp_client_;
  FtpServer ftp_server_;
  
  void handle_root_();
  void handle_file_();
  void setup_ftp_();
  void setup_http_();
  bool fetch_file_(const std::string &path, std::string &content);
};

}  // namespace ftp_http_proxy
}  // namespace esphome
