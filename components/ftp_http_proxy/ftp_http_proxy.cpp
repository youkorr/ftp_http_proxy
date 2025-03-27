#include "ftp_http_proxy.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"

namespace esphome {
namespace ftp_http_proxy {

static const char* TAG = "ftp_http_proxy";

// Initialisation du proxy
void FTPHTTPProxy::setup() {
  // Initialiser SPIFFS
  esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
  };
  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to mount SPIFFS");
      return;
  }

  // Démarrer le serveur HTTP
  start_http_server();
  ESP_LOGD(TAG, "FTP HTTP Proxy started on port %d", local_port_);
}

// Boucle principale
void FTPHTTPProxy::loop() {
  // Gestion des requêtes HTTP entrantes
  // Rien à faire ici car le serveur HTTP gère les requêtes via des callbacks
}

// Démarrer le serveur HTTP
void FTPHTTPProxy::start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;

  if (httpd_start(&http_server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  // Enregistrer le gestionnaire de requêtes HTTP
  httpd_uri_t uri_handler = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = [](httpd_req_t *req) -> esp_err_t {
          FTPHTTPProxy* instance = reinterpret_cast<FTPHTTPProxy*>(req->user_ctx);
          instance->handle_http_request(req);
          return ESP_OK;
      },
      .user_ctx = this
  };
  httpd_register_uri_handler(http_server_, &uri_handler);

  ESP_LOGD(TAG, "HTTP server started on port %d", local_port_);
}

// Gérer les requêtes HTTP
void FTPHTTPProxy::handle_http_request(httpd_req_t *req) {
  // Extraire le nom de fichier de la requête
  char filename[128];
  if (sscanf(req->uri, "/%s", filename) != 1) {
    ESP_LOGE(TAG, "Invalid request URI");
    httpd_resp_send_500(req);
    return;
  }

  // Télécharger le fichier depuis FTP si nécessaire
  if (!download_ftp_file(filename)) {
    ESP_LOGE(TAG, "Failed to download file from FTP: %s", filename);
    httpd_resp_send_500(req);
    return;
  }

  // Servir le fichier local
  FILE *file = fopen(("/spiffs/" + std::string(filename)).c_str(), "r");
  if (!file) {
    ESP_LOGE(TAG, "File not found: %s", filename);
    httpd_resp_send_404(req);
    return;
  }

  // Envoyer la réponse HTTP
  httpd_resp_set_type(req, "application/octet-stream");

  char buffer[512];
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    httpd_resp_send_chunk(req, buffer, bytes_read);
  }
  fclose(file);

  httpd_resp_send_chunk(req, NULL, 0); // Fin de la réponse
}

// Télécharger un fichier depuis FTP
bool FTPHTTPProxy::download_ftp_file(const std::string& filename) {
  std::string remote_path = find_remote_path_for_file(filename);
  if (remote_path.empty()) {
    ESP_LOGE(TAG, "No remote path found for file: %s", filename.c_str());
    return false;
  }

  // Établir une connexion FTP via socket
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket");
    return false;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  inet_pton(AF_INET, ftp_server_.c_str(), &server_addr.sin_addr);

  if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "FTP connection failed");
    close(sock);
    return false;
  }

  // Authentification FTP (exemple simplifié)
  send(sock, ("USER " + username_ + "\r\n").c_str(), strlen(("USER " + username_ + "\r\n").c_str()), 0);
  send(sock, ("PASS " + password_ + "\r\n").c_str(), strlen(("PASS " + password_ + "\r\n").c_str()), 0);

  // Commande pour récupérer le fichier
  send(sock, ("RETR " + remote_path + "/" + filename + "\r\n").c_str(), strlen(("RETR " + remote_path + "/" + filename + "\r\n").c_str()), 0);

  // TODO: Implémenter le téléchargement réel du fichier
  close(sock);
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
