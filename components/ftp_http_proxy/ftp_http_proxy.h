#pragma once

#include "esphome.h"
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <netdb.h>
#include <vector>
#include <string>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void set_ftp_server(const std::string &server) { ftp_server_ = server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void add_remote_path(const std::string &path) { remote_paths_.push_back(path); }
  void set_local_port(uint16_t port) { local_port_ = port; }
  void set_ftp_port(uint16_t port) { ftp_port_ = port; }
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

 protected:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  std::vector<std::string> remote_paths_;
  uint16_t local_port_{8000};
  uint16_t ftp_port_{21};
  httpd_handle_t server_{nullptr};
  int sock_{-1};

  // Added method declarations to resolve compilation errors
 private:
  bool authenticate_ftp();
  std::string get_file_extension(const std::string& filename);
  const char* get_mime_type(const std::string& extension);
  static std::string remove_quotes(const std::string& str);

  bool connect_to_ftp();
  bool download_file(const std::string &remote_path, httpd_req_t *req);
  void setup_http_server();
  static esp_err_t http_req_handler(httpd_req_t *req);
};

}  // namespace ftp_http_proxy
}  // namespace esphome

