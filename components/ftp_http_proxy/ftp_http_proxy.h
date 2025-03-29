#pragma once

#include "esphome/core/component.h"
#include "esp_http_server.h"
#include <string>
#include <vector>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  
  void set_ftp_server(const std::string& server) { ftp_server_ = server; }
  void set_username(const std::string& username) { username_ = username; }
  void set_password(const std::string& password) { password_ = password; }
  void set_local_port(int port) { local_port_ = port; }
  
  // Ajouter un chemin distant à surveiller
  void add_remote_path(const std::string& path) { remote_paths_.push_back(path); }
  
  // Méthodes pour gérer la carte SD
  bool ensure_directory(const std::string& path);
  
  // Méthodes pour la gestion FTP
  bool connect_to_ftp();
  bool download_file(const std::string& remote_path, const std::string& local_path);
  bool list_directory(const std::string& path, std::string& content);
  void disconnect_ftp();
  
  // Fonction pour configurer le serveur HTTP
  void setup_http_server();
  
  // Gestionnaire HTTP statique
  static esp_err_t http_req_handler(httpd_req_t* req);
  
 protected:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  int local_port_{80};
  std::vector<std::string> remote_paths_;
  
  int sock_{-1};
  httpd_handle_t server_{nullptr};
};

}  // namespace ftp_http_proxy
}  // namespace esphome
