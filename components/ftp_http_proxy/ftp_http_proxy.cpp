#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/netdb.h>
#include <cstring>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance tasks can be added here if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);

  // DNS resolution
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS lookup failed");
    return false;
  }
  memcpy(&server_addr.sin_addr.s_addr, ftp_host->h_addr, ftp_host->h_length);

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed");
    return false;
  }

  // Set timeouts
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Connection failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[128];
  int len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Invalid FTP welcome");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentication
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send USER");
    close(sock_);
    sock_ = -1;
    return false;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "331 ")) {
    ESP_LOGE(TAG, "USER command failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASS");
    close(sock_);
    sock_ = -1;
    return false;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "Login failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Set binary mode
  snprintf(buffer, sizeof(buffer), "TYPE I\r\n");
  send(sock_, buffer, strlen(buffer), 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!connect_to_ftp()) {
    return false;
  }

  char buffer[256];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASV");
    close(sock_);
    sock_ = -1;
    return false;
  }

  int len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "PASV failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Parse PASV response
  int ip[4], port[2];
  char *ptr = strchr(buffer, '(');
  if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Create data connection
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket failed");
    close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connect failed");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Request file
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send RETR");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Transfer not started");
    close(data_sock);
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Stream data with chunked transfer
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

  char chunk[2048];
  size_t total = 0;
  while (true) {
    len = recv(data_sock, chunk, sizeof(chunk), 0);
    if (len <= 0) break;

    if (httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
      ESP_LOGE(TAG, "Chunk send failed");
      break;
    }
    total += len;
  }

  httpd_resp_send_chunk(req, NULL, 0);
  close(data_sock);

  // Verify transfer completion
  recv(sock_, buffer, sizeof(buffer) - 1, 0);
  send(sock_, "QUIT\r\n", 6, 0);
  close(sock_);
  sock_ = -1;

  ESP_LOGI(TAG, "Transferred %zu bytes", total);
  return total > 0;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string path = req->uri;

  // Remove leading slash
  if (path.size() > 1 && path[0] == '/') {
    path = path.substr(1);
  }

  ESP_LOGD(TAG, "Request for: %s", path.c_str());

  for (const auto &rp : proxy->remote_paths_) {
    if (path == rp) {
      if (proxy->download_file(rp, req)) {
        return ESP_OK;
      }
      break;
    }
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server start failed");
    return;
  }

  httpd_uri_t uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = http_req_handler,
      .user_ctx = this
  };

  httpd_register_uri_handler(server_, &uri);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome

