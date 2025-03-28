#include "ftp_http_proxy.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include <cstring>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance périodique si nécessaire
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS lookup failed");
    return false;
  }

  sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket");
    return false;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  if (recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "FTP welcome message not received");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentification
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0 || 
      recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "331 ")) {
    ESP_LOGE(TAG, "FTP username rejected");
    close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0 || 
      recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "FTP login failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Mode binaire
  snprintf(buffer, sizeof(buffer), "TYPE I\r\n");
  send(sock_, buffer, strlen(buffer), 0);
  recv(sock_, buffer, sizeof(buffer), 0);

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, std::string &content) {
  if (!connect_to_ftp()) {
    return false;
  }

  char buffer[512];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (send(sock_, buffer, strlen(buffer), 0) < 0 || 
      recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "FTP PASV failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Parse PASV response (ex: "227 Entering Passive Mode (192,168,1,100,123,45)")
  int ip1, ip2, ip3, ip4, port1, port2;
  if (sscanf(strchr(buffer, '('), "(%d,%d,%d,%d,%d,%d)", &ip1, &ip2, &ip3, &ip4, &port1, &port2) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response");
    close(sock_);
    sock_ = -1;
    return false;
  }

  int data_port = (port1 << 8) + port2;
  ESP_LOGD(TAG, "Data port: %d", data_port);

  // Connexion au port de données
  int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Failed to create data socket");
    close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip1 << 24) | (ip2 << 16) | (ip3 << 8) | ip4);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Failed to connect to data port");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Envoi de la commande RETR
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0 || 
      recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "FTP RETR failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Lecture des données
  content.clear();
  int bytes_received;
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
    content.append(buffer, bytes_received);
  }

  close(data_sock);
  recv(sock_, buffer, sizeof(buffer), 0); // Lire la réponse 226

  // Fermeture propre
  snprintf(buffer, sizeof(buffer), "QUIT\r\n");
  send(sock_, buffer, strlen(buffer), 0);
  close(sock_);
  sock_ = -1;

  return !content.empty();
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  httpd_uri_t uri_proxy = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = http_req_handler,
      .user_ctx = this};

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
