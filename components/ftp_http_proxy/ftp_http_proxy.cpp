#include "ftp_http_proxy.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "../sd_mmc_card/sd_mmc_card.h"

static const char *TAG = "ftp_http_proxy";

namespace esphome {
namespace ftp_http_proxy {

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string path = req->uri;
  std::string content;

  for (const auto &remote_path : proxy->remote_paths_) {
    if (path == "/" + remote_path || path == remote_path) {
      if (proxy->download_file(remote_path, content)) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, content.c_str(), content.length());
        return ESP_OK;
      }
    }
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Setting up FTP to HTTP proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Handle periodic tasks if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  // Implementation of FTP connection
  return false; // Placeholder
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, std::string &content) {
  // Implementation of file download
  return false; // Placeholder
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
