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
  // Attendre la connexion Wi-Fi
  while (WiFi.status() != WL_CONNECTED) {
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    delay(1000);
  }
  ESP_LOGI(TAG, "Wi-Fi connected. IP address: %s", WiFi.localIP().toString().c_str());

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
  ESP_LOGD(TAG, "SPIFFS mounted successfully");

  // Démarrer le serveur HTTP
  start_http_server();
  ESP_LOGD(TAG, "FTP HTTP Proxy started on port %d", local_port_);
}

// Boucle principale
void FTPHTTPProxy::loop() {
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
  ESP_LOGD(TAG, "Attempting to download file: %s", filename.c_str());

  // Étape 1 : Créer une socket pour la connexion de contrôle
  int control_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (control_sock < 0) {
    ESP_LOGE(TAG, "Failed to create control socket");
    return false;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21); // Port FTP standard
  inet_pton(AF_INET, ftp_server_.c_str(), &server_addr.sin_addr);

  ESP_LOGD(TAG, "Connecting to FTP server: %s", ftp_server_.c_str());
  if (connect(control_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "FTP connection failed");
    close(control_sock);
    return false;
  }

  ESP_LOGD(TAG, "Connected to FTP server. Authenticating...");

  // Étape 2 : Authentification FTP
  send(control_sock, ("USER " + username_ + "\r\n").c_str(), strlen(("USER " + username_ + "\r\n").c_str()), 0);
  delay(500);
  send(control_sock, ("PASS " + password_ + "\r\n").c_str(), strlen(("PASS " + password_ + "\r\n").c_str()), 0);
  delay(500);

  ESP_LOGD(TAG, "Authenticated. Sending PASV command...");

  // Étape 3 : Activer le mode passif (PASV)
  send(control_sock, "PASV\r\n", strlen("PASV\r\n"), 0);
  delay(500);

  // Lire la réponse du serveur après PASV
  char pasv_response[128];
  int bytes_read = recv(control_sock, pasv_response, sizeof(pasv_response) - 1, 0);
  if (bytes_read <= 0) {
    ESP_LOGE(TAG, "Failed to receive PASV response");
    close(control_sock);
    return false;
  }
  pasv_response[bytes_read] = '\0';
  ESP_LOGD(TAG, "PASV response: %s", pasv_response);

  // Extraire l'adresse IP et le port dynamique depuis la réponse PASV
  int ip1, ip2, ip3, ip4, port_high, port_low;
  if (sscanf(pasv_response, "%*[^0-9]%d,%d,%d,%d,%d,%d", &ip1, &ip2, &ip3, &ip4, &port_high, &port_low) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response");
    close(control_sock);
    return false;
  }

  uint16_t data_port = (port_high << 8) | port_low;
  ESP_LOGD(TAG, "Extracted data port: %d", data_port);

  // Étape 4 : Créer une nouvelle socket pour la connexion de données
  int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Failed to create data socket");
    close(control_sock);
    return false;
  }

  struct sockaddr_in data_addr;
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = ((uint32_t)ip1 << 24) | ((uint32_t)ip2 << 16) | ((uint32_t)ip3 << 8) | (uint32_t)ip4;

  ESP_LOGD(TAG, "Connecting to data port: %d", data_port);
  if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect to data port");
    close(control_sock);
    close(data_sock);
    return false;
  }

  ESP_LOGD(TAG, "Connected to data port. Sending RETR command...");

  // Étape 5 : Envoyer la commande RETR pour récupérer le fichier
  std::string retr_command = "RETR " + find_remote_path_for_file(filename) + "/" + filename + "\r\n";
  send(control_sock, retr_command.c_str(), strlen(retr_command.c_str()), 0);
  delay(500);

  // Étape 6 : Télécharger le fichier depuis la connexion de données
  FILE *file = fopen(("/spiffs/" + filename).c_str(), "w");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open local file for writing");
    close(control_sock);
    close(data_sock);
    return false;
  }

  char buffer[512];
  while ((bytes_read = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
    fwrite(buffer, 1, bytes_read, file);
  }
  fclose(file);

  ESP_LOGD(TAG, "File downloaded successfully");

  // Fermer les sockets
  close(data_sock);
  close(control_sock);

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
