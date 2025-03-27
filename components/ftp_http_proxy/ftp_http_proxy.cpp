# Fichier custom_components/ftp_http_proxy/ftp_http_proxy.cpp
#include "ftp_http_proxy.h"
#include "esphome/core/log.h"
#include <WiFiClient.h>
#include <FS.h>

namespace esphome {
namespace ftp_http_proxy {

static const char* TAG = "ftp_http_proxy";

void FTPHTTPProxy::setup() {
  // Initialisation du serveur HTTP
  http_server_.begin();
  ESP_LOGD(TAG, "FTP HTTP Proxy started on port %d", local_port_);
}

void FTPHTTPProxy::loop() {
  // Gestion des requêtes HTTP entrantes
  WiFiClient client = http_server_.available();
  if (client) {
    handle_http_request();
  }
}

bool FTPHTTPProxy::download_ftp_file(const std::string& filename) {
  // Implémentation du téléchargement FTP
  WiFiClient ftp_client;
  if (!ftp_client.connect(ftp_server_.c_str(), 21)) {
    ESP_LOGE(TAG, "FTP connection failed");
    return false;
  }
  
  // Authentification et téléchargement (à implémenter)
  // Exemple simplifié, nécessite une vraie implémentation FTP
  
  return true;
}

void FTPHTTPProxy::handle_http_request() {
  WiFiClient client = http_server_.available();
  if (!client) return;
  
  // Lire la requête HTTP
  String request = client.readStringUntil('\r');
  client.flush();
  
  // Extraire le nom de fichier de la requête
  int file_start = request.indexOf("GET /") + 4;
  int file_end = request.indexOf(" ", file_start);
  String filename = request.substring(file_start, file_end);
  
  // Télécharger le fichier depuis FTP si nécessaire
  if (download_ftp_file(filename.c_str())) {
    // Servir le fichier local
    File file = SPIFFS.open("/" + filename, "r");
    if (file) {
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/octet-stream");
      client.println();
      
      while (file.available()) {
        client.write(file.read());
      }
      file.close();
    } else {
      client.println("HTTP/1.1 404 Not Found");
    }
  } else {
    client.println("HTTP/1.1 500 Internal Server Error");
  }
  
  client.stop();
}

}  // namespace ftp_http_proxy
}  // namespace esphome
