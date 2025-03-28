#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Starting FTP to HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = lwip_gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS lookup failed");
    return false;
  }

  sock_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
    return false;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (lwip_connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Connection failed: errno %d", errno);
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int len = lwip_recv(sock_, buffer, sizeof(buffer), 0);
  if (len <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Invalid FTP welcome");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "331 ")) {
    ESP_LOGE(TAG, "USER command failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "PASS command failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "TYPE I\r\n");
  lwip_send(sock_, buffer, strlen(buffer), 0);
  lwip_recv(sock_, buffer, sizeof(buffer), 0);

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!connect_to_ftp()) return false;

  char buffer[512];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "PASV failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  int ip[4], port[2];
  char *ptr = strchr(buffer, '(');
  if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "PASV parse error");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  int data_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (lwip_connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connect failed");
    lwip_close(data_sock);
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "RETR failed");
    lwip_close(data_sock);
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

  char chunk[4096];
  size_t total = 0;
  while (true) {
    int len = lwip_recv(data_sock, chunk, sizeof(chunk), 0);
    if (len <= 0) break;

    if (httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
      ESP_LOGE(TAG, "Chunk send failed");
      break;
    }
    total += len;
  }

  httpd_resp_send_chunk(req, NULL, 0);
  lwip_close(data_sock);

  lwip_recv(sock_, buffer, sizeof(buffer), 0); // Wait for 226
  snprintf(buffer, sizeof(buffer), "QUIT\r\n");
  lwip_send(sock_, buffer, strlen(buffer), 0);
  lwip_close(sock_);
  sock_ = -1;

  ESP_LOGI(TAG, "Transferred %d bytes", total);
  return total > 0;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string path = req->uri;

  if (path.size() > 1 && path[0] == '/') {
    path = path.substr(1);
  }

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

