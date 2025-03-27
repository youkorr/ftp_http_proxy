#pragma once

#include "esphome/core/component.h"
#include "esphome/components/media_player/media_player.h"
#include <WiFiClient.h>
#include <WiFiServer.h>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  
  void set_ftp_server(std::string server) { ftp_server_ = server; }
  void set_username(std::string username) { username_ = username; }
  void set_password(std::string password) { password_ = password; }
  void set_remote_path(std::string path) { remote_path_ = path; }
  void set_local_port(uint16_t port) { local_port_ = port; }

 private:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  std::string remote_path_;
  uint16_t local_port_ = 8000;
  
  WiFiServer http_server_{local_port_};
  
  bool download_ftp_file(const std::string& filename);
  void handle_http_request();
};

}  // namespace ftp_http_proxy
}  // namespace esphome
