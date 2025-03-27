
#pragma once
#include "esphome/core/component.h"
#include "esphome/components/media_player/media_player.h"
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <vector>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  
  void set_ftp_server(std::string server) { ftp_server_ = server; }
  void set_username(std::string username) { username_ = username; }
  void set_password(std::string password) { password_ = password; }
  
  // Nouvelle méthode pour ajouter des chemins distants
  void add_remote_path(std::string path) { remote_paths_.push_back(path); }
  
  void set_local_port(uint16_t port) { local_port_ = port; }
 private:
  std::string ftp_server_;
  std::string username_;
  std::string password_;
  
  // Vecteur de chemins distants
  std::vector<std::string> remote_paths_;
  
  uint16_t local_port_ = 8000;
  
  WiFiServer http_server_{local_port_};
  
  bool download_ftp_file(const std::string& filename);
  void handle_http_request();
  
  // Nouvelle méthode pour trouver le chemin distant pour un fichier
  std::string find_remote_path_for_file(const std::string& filename);
};
}  // namespace ftp_http_proxy
}  // namespace esphome
