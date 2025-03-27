#include "ftp_http_proxy.h"
#include "esphome/core/log.h"
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <FS.h>

namespace esphome {
namespace ftp_http_proxy {

static const char* TAG = "ftp_http_proxy";

// Initialisation du proxy
void FTPHTTPProxy::setup() {
  // Démarrer le serveur HTTP
  start_http_server();
  ESP_LOGD(TAG, "FTP HTTP Proxy started on port %d", local_port_);
}

// Boucle principale
void FTPHTTPProxy::loop() {
  // Gestion des requêtes HTTP entrantes
  handle_http_request();
}

// Démarrer le serveur HTTP
void FTPHTTPProxy::start_http_server() {
  http_server_ = new WiFiServer(local_port_);
  http_server_->begin();
  ESP_LOGD(TAG, "HTTP server started on port %d", local_port_);
}

// Gérer les requêtes HTTP
void FTPHTTPProxy::handle_http_request() {
  WiFiClient client = http_server_->available();
  if (!client) return;

  // Lire la requête HTTP
  String request = client.readStringUntil('\r');
  client.flush();

  // Extraire le nom de fichier de la requête
  int file_start = request.indexOf("GET /") + 5;
  int file_end = request.indexOf(" ", file_start);
  String filename = request.substring(file_start, file_end);

  if (filename.isEmpty()) {
    ESP_LOGE(TAG, "Invalid filename in request");
    client.println("HTTP/1.1 400 Bad Request");
    client.stop();
    return;
  }

  // Télécharger le fichier depuis FTP si nécessaire
  if (!download_ftp_file(filename.c_str())) {
    ESP_LOGE(TAG, "Failed to download file from FTP: %s", filename.c_str());
    client.println("HTTP/1.1 500 Internal Server Error");
    client.stop();
    return;
  }

  // Servir le fichier local
  File file = SPIFFS.open("/" + filename, "r");
  if (!file) {
    ESP_LOGE(TAG, "File not found: %s", filename.c_str());
    client.println("HTTP/1.1 404 Not Found");
    client.stop();
    return;
  }

  // Envoyer la réponse HTTP
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/octet-stream");
  client.println();

  while (file.available()) {
    client.write(file.read());
  }
  file.close();

  client.stop();
}

// Télécharger un fichier depuis FTP
bool FTPHTTPProxy::download_ftp_file(const std::string& filename) {
  std::string remote_path = find_remote_path_for_file(filename);
  if (remote_path.empty()) {
    ESP_LOGE(TAG, "No remote path found for file: %s", filename.c_str());
    return false;
  }

  // Implémentation simplifiée du téléchargement FTP
  WiFiClient ftp_client;
  if (!ftp_client.connect(ftp_server_.c_str(), 21)) {
    ESP_LOGE(TAG, "FTP connection failed");
    return false;
  }

  // Authentification FTP (exemple simplifié)
  ftp_client.println("USER " + username_);
  ftp_client.println("PASS " + password_);

  // Commande pour récupérer le fichier
  ftp_client.println("RETR " + remote_path + "/" + filename);

  // TODO: Implémenter le téléchargement réel du fichier
  // Pour l'instant, nous supposons que le fichier est téléchargé avec succès

  ftp_client.stop();
  return true;
}

// Trouver le chemin distant pour un fichier donné
std::string FTPHTTPProxy::find_remote_path_for_file(const std::string& filename) {
  for (const auto& path : remote_paths_) {
    // Logique simple pour vérifier si le fichier existe dans ce chemin
    // Ici, nous supposons que tous les chemins sont valides
    return path;
  }
  return ""; // Aucun chemin trouvé
}

}  // namespace ftp_http_proxy
}  // namespace esphome
