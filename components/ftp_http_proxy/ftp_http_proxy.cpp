#include "ftp_http_proxy.h"
#include "esphome/core/log.h"
#include <string>
#include <vector>
#include <esp_http_server.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_http_proxy";

static std::string remove_quotes(const std::string& str) {
  if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
    return str.substr(1, str.size() - 2);
  }
  return str;
}

FTPHTTPProxy::FTPHTTPProxy() : ftp_socket_(-1), server_(nullptr), setup_complete_(false) {
  // Initialisation des valeurs par défaut
  this->ftp_port_ = 21;
  this->local_port_ = 80;
}

void FTPHTTPProxy::setup() {
  // Vérifier si déjà configuré
  if (setup_complete_) {
    ESP_LOGW(TAG, "Setup already completed");
    return;
  }
  
  // Attendre que le WiFi soit connecté
  if (wifi_sta_status() != WIFI_STA_CONNECTED) {
    ESP_LOGW(TAG, "WiFi not connected yet, delaying HTTP server start");
    // La configuration sera tentée à nouveau lors d'un appel ultérieur à setup()
    return;
  }
  
  // Attendre que la pile TCP/IP soit prête
  vTaskDelay(pdMS_TO_TICKS(1000));
  
  // Configuration du serveur HTTP
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;  // Augmenter la taille de la pile
  config.task_priority = tskIDLE_PRIORITY + 5;  // Priorité appropriée
  config.core_id = 0;  // Exécuter sur le cœur 0 (si CPU double cœur)
  config.lru_purge_enable = true;  // Activer la purge LRU pour éviter les fuites mémoire
  
  ESP_LOGI(TAG, "Starting HTTP server on port %d", this->local_port_);
  esp_err_t ret = httpd_start(&server_, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
    return;
  }

  // Configuration de la gestion des requêtes HTTP
  httpd_uri_t uri_config = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = http_req_handler,
      .user_ctx = this
  };

  if (httpd_register_uri_handler(server_, &uri_config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register URI handler");
    httpd_stop(server_);
    server_ = nullptr;
    return;
  }
  
  setup_complete_ = true;
  ESP_LOGI(TAG, "HTTP server setup complete");
}

void FTPHTTPProxy::loop() {
  // Si le serveur n'est pas encore configuré, essayer à nouveau
  if (!setup_complete_) {
    setup();
  }
}

void FTPHTTPProxy::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP HTTP Proxy:");
  ESP_LOGCONFIG(TAG, "  FTP Server: %s", this->ftp_server_.c_str());
  ESP_LOGCONFIG(TAG, "  FTP Port: %d", this->ftp_port_);
  ESP_LOGCONFIG(TAG, "  Local Port: %d", this->local_port_);
  ESP_LOGCONFIG(TAG, "  Username: %s", this->username_.c_str());
  ESP_LOGCONFIG(TAG, "  Configured Remote Paths: %d", this->remote_paths_.size());
}

void FTPHTTPProxy::set_ftp_port(uint16_t port) {
  this->ftp_port_ = port;
}

void FTPHTTPProxy::set_credentials(const std::string& username, const std::string& password) {
  this->username_ = username;
  this->password_ = password;
}

bool FTPHTTPProxy::connect_to_ftp() {
  if (ftp_socket_ > 0) {
    close(ftp_socket_);
    ftp_socket_ = -1;
  }

  ftp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (ftp_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket");
    return false;
  }

  // Définir un délai d'attente pour le socket
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(ftp_socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(ftp_socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(this->ftp_port_);

  if (inet_pton(AF_INET, this->ftp_server_.c_str(), &server_addr.sin_addr) <= 0) {
    ESP_LOGE(TAG, "Invalid FTP server address");
    close(ftp_socket_);
    ftp_socket_ = -1;
    return false;
  }

  if (connect(ftp_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect to FTP server");
    close(ftp_socket_);
    ftp_socket_ = -1;
    return false;
  }

  // Recevoir le message de bienvenue
  char response[1024];
  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No welcome message from FTP server");
    close(ftp_socket_);
    ftp_socket_ = -1;
    return false;
  }

  if (!authenticate_ftp()) {
    close(ftp_socket_);
    ftp_socket_ = -1;
    return false;
  }

  return true;
}

bool FTPHTTPProxy::authenticate_ftp() {
  // Envoyer le nom d'utilisateur
  std::string user_cmd = "USER " + this->username_ + "\r\n";
  if (send(ftp_socket_, user_cmd.c_str(), user_cmd.size(), 0) <= 0) {
    ESP_LOGE(TAG, "Failed to send USER command");
    return false;
  }

  // Attendre la réponse
  char response[1024];
  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No response to USER command");
    return false;
  }

  // Réponse 331 signifie que le mot de passe est requis
  if (strncmp(response, "331", 3) != 0) {
    ESP_LOGE(TAG, "Invalid username or unexpected response: %s", response);
    return false;
  }

  // Envoyer le mot de passe
  std::string pass_cmd = "PASS " + this->password_ + "\r\n";
  if (send(ftp_socket_, pass_cmd.c_str(), pass_cmd.size(), 0) <= 0) {
    ESP_LOGE(TAG, "Failed to send PASS command");
    return false;
  }

  // Attendre la réponse
  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No response to PASS command");
    return false;
  }

  // Réponse 230 signifie authentifié avec succès
  if (strncmp(response, "230", 3) != 0) {
    ESP_LOGE(TAG, "Authentication failed: %s", response);
    return false;
  }

  ESP_LOGI(TAG, "Successfully authenticated to FTP server");
  return true;
}

bool FTPHTTPProxy::download_file(const std::string& remote_path, httpd_req_t* req) {
  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Failed to connect to FTP server");
    httpd_resp_send_404(req);
    return false;
  }

  // Passer en mode binaire
  const char* type_cmd = "TYPE I\r\n";
  if (send(ftp_socket_, type_cmd, strlen(type_cmd), 0) <= 0) {
    ESP_LOGE(TAG, "Failed to send TYPE command");
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  char response[1024];
  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No response to TYPE command");
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Configuration du mode passif
  const char* pasv_cmd = "PASV\r\n";
  if (send(ftp_socket_, pasv_cmd, strlen(pasv_cmd), 0) <= 0) {
    ESP_LOGE(TAG, "Failed to send PASV command");
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No response to PASV command");
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Analyser la réponse PASV pour obtenir l'adresse IP et le port
  int h1, h2, h3, h4, p1, p2;
  if (sscanf(response, "%*[^(](%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response: %s", response);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Calculer le port de données
  int data_port = p1 * 256 + p2;
  char data_ip[32];
  sprintf(data_ip, "%d.%d.%d.%d", h1, h2, h3, h4);

  // Se connecter au port de données
  int data_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (data_socket < 0) {
    ESP_LOGE(TAG, "Failed to create data socket");
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Définir un délai d'attente pour le socket de données aussi
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(data_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(data_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  if (inet_pton(AF_INET, data_ip, &data_addr.sin_addr) <= 0) {
    ESP_LOGE(TAG, "Invalid data IP address");
    close(data_socket);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  if (connect(data_socket, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect to data port");
    close(data_socket);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Demander le fichier
  std::string retr_cmd = "RETR " + remote_path + "\r\n";
  if (send(ftp_socket_, retr_cmd.c_str(), retr_cmd.size(), 0) <= 0) {
    ESP_LOGE(TAG, "Failed to send RETR command");
    close(data_socket);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No response to RETR command");
    close(data_socket);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_500(req);
    return false;
  }

  // Si le début de la réponse n'est pas 150, le téléchargement a échoué
  if (strncmp(response, "150", 3) != 0) {
    ESP_LOGE(TAG, "Failed to retrieve file: %s", response);
    close(data_socket);
    close(ftp_socket_);
    ftp_socket_ = -1;
    httpd_resp_send_404(req);
    return false;
  }

  // Définir l'en-tête Content-Type approprié
  std::string file_ext = get_file_extension(remote_path);
  const char* mime_type = get_mime_type(file_ext);
  httpd_resp_set_type(req, mime_type);

  // Lire les données et les envoyer
  char buffer[4096];
  int bytes_read;
  while ((bytes_read = recv(data_socket, buffer, sizeof(buffer), 0)) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to send response chunk");
      break;
    }
  }

  // Fin de la réponse
  httpd_resp_send_chunk(req, nullptr, 0);

  // Fermer les sockets
  close(data_socket);
  
  // Attendre la confirmation du serveur FTP
  memset(response, 0, sizeof(response));
  if (recv(ftp_socket_, response, sizeof(response) - 1, 0) <= 0) {
    ESP_LOGE(TAG, "No completion response from FTP server");
  }

  close(ftp_socket_);
  ftp_socket_ = -1;

  return true;
}

std::string FTPHTTPProxy::get_file_extension(const std::string& filename) {
  size_t pos = filename.find_last_of('.');
  return (pos != std::string::npos) ? filename.substr(pos + 1) : "";
}

const char* FTPHTTPProxy::get_mime_type(const std::string& extension) {
  if (extension == "html" || extension == "htm")
    return "text/html";
  if (extension == "css")
    return "text/css";
  if (extension == "js")
    return "application/javascript";
  if (extension == "jpg" || extension == "jpeg")
    return "image/jpeg";
  if (extension == "png")
    return "image/png";
  if (extension == "gif")
    return "image/gif";
  if (extension == "ico")
    return "image/x-icon";
  if (extension == "xml")
    return "text/xml";
  if (extension == "pdf")
    return "application/pdf";
  if (extension == "zip")
    return "application/zip";
  if (extension == "json")
    return "application/json";
  return "application/octet-stream";  // Type par défaut
}

wifi_sta_status_t FTPHTTPProxy::wifi_sta_status() {
  wifi_ap_record_t info;
  if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
    return WIFI_STA_CONNECTED;
  }
  return WIFI_STA_DISCONNECTED;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t* req) {
  FTPHTTPProxy* proxy = static_cast<FTPHTTPProxy*>(req->user_ctx);
  if (!proxy) {
    return ESP_FAIL;
  }

  // Obtenir le chemin de la requête
  std::string uri(req->uri);
  ESP_LOGI(TAG, "Received HTTP request for %s", uri.c_str());

  // Chemin par défaut
  if (uri == "/") {
    uri = "/index.html";
  }

  // Vérifier si le chemin est dans la liste des chemins autorisés
  bool path_found = false;
  std::string remote_path;

  for (const std::string& allowed_path : proxy->remote_paths_) {
    std::string clean_allowed_path = remove_quotes(allowed_path);
    if (uri.find(clean_allowed_path) == 0) {
      path_found = true;
      remote_path = uri;
      break;
    }
  }

  if (!path_found) {
    ESP_LOGE(TAG, "Path not in allowed list: %s", uri.c_str());
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  // Télécharger et envoyer le fichier
  if (!proxy->download_file(remote_path, req)) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

}  // namespace ftp_http_proxy
}  // namespace esphome
