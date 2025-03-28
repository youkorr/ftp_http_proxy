#pragma once

#include "esphome/core/component.h"
#include <string>
#include <vector>
#include <esp_http_server.h>

namespace esphome {
namespace ftp_http_proxy {

// Enum pour l'état de la connexion WiFi
enum wifi_sta_status_t {
  WIFI_STA_DISCONNECTED = 0,
  WIFI_STA_CONNECTED
};

class FTPHTTPProxy : public Component {
 public:
  FTPHTTPProxy();
  
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Méthodes de configuration
  void set_ftp_server(const std::string& server) { this->ftp_server_ = server; }
  void set_ftp_port(uint16_t port);
  void set_local_port(uint16_t port) { this->local_port_ = port; }
  void set_credentials(const std::string& username, const std::string& password);
  void add_remote_path(const std::string& path) { this->remote_paths_.push_back(path); }

 protected:
  // Méthodes pour la communication FTP
  bool connect_to_ftp();
  bool authenticate_ftp();
  bool download_file(const std::string& remote_path, httpd_req_t* req);
  
  // Méthodes utilitaires
  std::string get_file_extension(const std::string& filename);
  const char* get_mime_type(const std::string& extension);
  wifi_sta_status_t wifi_sta_status();

  // Handler pour les requêtes HTTP
  static esp_err_t http_req_handler(httpd_req_t* req);

  // Attributs
  std::string ftp_server_;
  uint16_t ftp_port_;
  uint16_t local_port_;
  std::string username_;
  std::string password_;
  std::vector<std::string> remote_paths_;
  
  int ftp_socket_;
  httpd_handle_t server_;
  bool setup_complete_;  // Nouveau attribut pour suivre l'état de configuration
};

}  // namespace ftp_http_proxy
}  // namespace esphome
