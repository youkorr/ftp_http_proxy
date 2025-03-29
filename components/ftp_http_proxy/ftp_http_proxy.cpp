#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "../ftp_server/ftp_server.h"
#include "../sd_mmc_card/sd_mmc_card.h"


static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance tasks if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS lookup failed");
    return false;
  }

  sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
    return false;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (::connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed: errno %d", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "FTP welcome message not received");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentication
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send USER command");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "331 ")) {
    ESP_LOGE(TAG, "FTP username rejected");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASS command");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "FTP login failed");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Binary mode
  snprintf(buffer, sizeof(buffer), "TYPE I\r\n");
  send(sock_, buffer, strlen(buffer), 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, std::string &content) {
  if (!connect_to_ftp()) {
    return false;
  }

  char buffer[512];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASV command");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "FTP PASV failed");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Parse PASV response
  int ip[4], port[2];
  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start || sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", 
                           &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV response");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  int data_port = port[0] * 256 + port[1];
  ESP_LOGD(TAG, "Data connection: %d.%d.%d.%d:%d", 
           ip[0], ip[1], ip[2], ip[3], data_port);

  // Create data socket
  int data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Failed to create data socket: errno %d", errno);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (::connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Failed to connect to data port: errno %d", errno);
    ::close(data_sock);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Send RETR command
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send RETR command");
    ::close(data_sock);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "FTP RETR failed");
    ::close(data_sock);
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Receive file content
  content.clear();
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    httpd_resp_send_chunk(seq, buffer, bytes_received);
  }

  ::close(data_sock);

  // Wait for transfer complete
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "226 ")) {
    ESP_LOGE(TAG, "File transfer not completed");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Quit
  snprintf(buffer, sizeof(buffer), "QUIT\r\n");
  send(sock_, buffer, strlen(buffer), 0);
  ::close(sock_);
  sock_ = -1;

  return !content.empty();
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;

  // Remove leading slash
  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGD(TAG, "Requested path: %s", requested_path.c_str());

  // Check against configured paths
  for (const auto &configured_path : proxy->remote_paths_) {
    ESP_LOGD(TAG, "Checking against: %s", configured_path.c_str());
    
    if (requested_path == configured_path) {
      std::string content;
      if (proxy->download_file(configured_path, content)) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, content.c_str(), content.length());
        return ESP_OK;
      }
      break;
    }
  }

  ESP_LOGE(TAG, "File not found: %s", requested_path.c_str());
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
  return ESP_FAIL;
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
      .user_ctx = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
