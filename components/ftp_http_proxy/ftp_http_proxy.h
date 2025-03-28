#pragma once

#include "esphome/core/component.h"
#include <string>
#include <vector>
#include <esp_http_server.h>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  FTPHTTPProxy();
  void setup() override;
  
  // Méthodes inline déjà définies dans le .h
  void set_ftp_server(const std::string &server) { ftp_server_ = server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }

  void add_remote_path(const std::string &path) { remote_paths_.push_back(path); }
  void set_local_port(uint16_t port) { local_port_ = port; }
  
  // Méthodes à définir dans le .cpp
  void set_ftp_port(uint16_t port);
  void set_credentials(const std::string& username, const std::string& password);
  
 protected:
  static esp_err_t http_req_handler(httpd_req_t* req);
  bool connect_to_ftp();
  bool authenticate_ftp();
  bool download_file(const std::string& remote_path, httpd_req_t* req);
  std::string get_file_extension(const std::string& filename);
  const char* get_mime_type(const std::string& extension);
  
  std::string ftp_server_;
  uint16_t ftp_port_;
  std::string username_;
  std::string password_;
  std::vector<std::string> remote_paths_;
  uint16_t local_port_;
  httpd_handle_t server_{nullptr};
  int ftp_socket_{-1};
};

}  // namespace ftp_http_proxy
}  // namespace esphome
