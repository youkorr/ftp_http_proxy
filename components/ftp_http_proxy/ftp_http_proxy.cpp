#include "ftp_http_proxy.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
  
  // Démarrer le serveur HTTP dans un thread séparé
  if (xTaskCreate(
        [](void *arg) {
          static_cast<FTPHTTPProxy*>(arg)->setup_http_server();
          vTaskDelete(NULL);
        },
        "http_server",
        8192,
        this,
        5,
        NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create HTTP server task");
  }
}

void FTPHTTPProxy::loop() {
  vTaskDelay(pdMS_TO_TICKS(100));
}

bool FTPHTTPProxy::connect_to_ftp() {
  ESP_LOGD(TAG, "Connecting to FTP server...");

  struct addrinfo hints = {};
  struct addrinfo *res;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(ftp_server_.c_str(), "21", &hints, &res);
  if (err != 0 || res == NULL) {
    ESP_LOGE(TAG, "DNS lookup failed: %s", err == EAI_SYSTEM ? strerror(errno) : gai_strerror(err));
    return false;
  }

  sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed: %s", strerror(errno));
    freeaddrinfo(res);
    return false;
  }

  // Configuration des timeouts
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
    ESP_LOGE(TAG, "Connection failed: %s", strerror(errno));
    close(sock_);
    freeaddrinfo(res);
    sock_ = -1;
    return false;
  }
  freeaddrinfo(res);

  char buffer[128];
  ssize_t len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Invalid FTP welcome");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentification
  const char *commands[] = {
    "USER", username_.c_str(),
    "PASS", password_.c_str(),
    "TYPE I", NULL
  };

  for (int i = 0; commands[i] != NULL; i += 2) {
    snprintf(buffer, sizeof(buffer), "%s %s\r\n", commands[i], commands[i+1]);
    if (send(sock_, buffer, strlen(buffer), 0) < 0) {
      ESP_LOGE(TAG, "Failed to send %s", commands[i]);
      close(sock_);
      sock_ = -1;
      return false;
    }

    len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (len <= 0) {
      ESP_LOGE(TAG, "No response to %s", commands[i]);
      close(sock_);
      sock_ = -1;
      return false;
    }
    buffer[len] = '\0';
  }

  return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  ESP_LOGD(TAG, "Starting download of %s", remote_path.c_str());

  if (!connect_to_ftp()) {
    return false;
  }

  char buffer[256];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASV");
    goto cleanup;
  }

  ssize_t len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Invalid PASV response");
    goto cleanup;
  }

  // Parse PASV response
  int ip[4], port[2];
  char *ptr = strchr(buffer, '(');
  if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Failed to parse PASV");
    goto cleanup;
  }

  // Data connection
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket failed: %s", strerror(errno));
    goto cleanup;
  }

  struct sockaddr_in data_addr = {};
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connect failed: %s", strerror(errno));
    close(data_sock);
    goto cleanup;
  }

  // Request file
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (send(sock_, buffer, strlen(buffer), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send RETR");
    close(data_sock);
    goto cleanup;
  }

  len = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (len <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Transfer not started");
    close(data_sock);
    goto cleanup;
  }

  // Stream data with chunked transfer
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

  size_t total = 0;
  while (true) {
    len = recv(data_sock, buffer, sizeof(buffer), 0);
    if (len <= 0) break;

    if (httpd_resp_send_chunk(req, buffer, len) != ESP_OK) {
      ESP_LOGE(TAG, "Chunk send failed");
      break;
    }
    total += len;
    ESP_LOGD(TAG, "Sent chunk: %d bytes (total: %d)", len, total);
  }

  close(data_sock);
  httpd_resp_send_chunk(req, NULL, 0);

  // Verify transfer completion
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

cleanup:
  if (sock_ >= 0) {
    send(sock_, "QUIT\r\n", 6, 0);
    close(sock_);
    sock_ = -1;
  }

  ESP_LOGI(TAG, "Transfer completed: %zu bytes", total);
  return total > 0;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string path = req->uri;

  // Normalize path
  if (path.size() > 1 && path[0] == '/') {
    path = path.substr(1);
  }

  ESP_LOGD(TAG, "Request for: %s", path.c_str());

  for (const auto &rp : proxy->remote_paths_) {
    if (path == rp) {
      ESP_LOGI(TAG, "Processing file: %s", rp.c_str());
      if (proxy->download_file(rp, req)) {
        return ESP_OK;
      }
      break;
    }
  }

  ESP_LOGE(TAG, "File not found: %s", path.c_str());
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  ESP_LOGI(TAG, "Starting HTTP server on port %d", local_port_);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 8192;
  config.max_uri_handlers = 5;
  config.backlog_conn = 2;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  httpd_uri_t uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = http_req_handler,
      .user_ctx = this
  };

  if (httpd_register_uri_handler(server_, &uri) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register URI handler");
    httpd_stop(server_);
    server_ = nullptr;
    return;
  }

  ESP_LOGI(TAG, "HTTP server started successfully");
}

}  // namespace ftp_http_proxy
}  // namespace esphome

