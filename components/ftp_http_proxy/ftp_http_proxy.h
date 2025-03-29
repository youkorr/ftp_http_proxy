#pragma once

#include "esphome/components/component.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESP32FtpServer.h>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  
  void set_ftp_server(const std::string &amp;server) { ftp_server_ = server; }
  void set_username(const std::string &amp;username) { username_ = username; }
  void set_password(const std::string &amp;password) { password_ = password; }
  void add_remote_path(const std::string &amp;path) { remote_paths_.push_back(path); }
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
  bool fetch_file_(const std::string &amp;path, std::string &amp;content);
};

}  // namespace ftp_http_proxy
}  // namespace esphome
