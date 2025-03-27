#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <vector>
#include <string>

namespace esphome {
namespace ftp_http_proxy {

class FTPHTTPProxy : public Component {
 public:
  void setup() override;
  void loop() override;

  // Configuration des paramètres FTP
  void set_ftp_server(const std::string& server) { ftp_server_ = server; }
  void set_username(const std::string& username) { username_ = username; }
  void set_password(const std::string& password) { password_ = password; }

  // Ajout de chemins distants
  void add_remote_path(const std::string& path) { remote_paths_.push_back(path); }

  // Configuration du port local
  void set_local_port(uint16_t port) { local_port_ = port; }

 private:
  std::string ftp_server_;
  std::string username_;
  std::string password_;

  // Liste des chemins distants
  std::vector<std::string> remote_paths_;

  // Port local pour le serveur HTTP
  uint16_t local_port_ = 8000;

  // Méthodes privées
  bool download_ftp_file(const std::string& filename);
  void handle_http_request();

  // Recherche du chemin distant pour un fichier donné
  std::string find_remote_path_for_file(const std::string& filename);

  // Serveur HTTP
  void start_http_server();
};

}  // namespace ftp_http_proxy
}  // namespace esphome
