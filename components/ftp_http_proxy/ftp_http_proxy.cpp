#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/stat.h>

static const char* TAG = "ftp_proxy";
constexpr size_t CHUNK_SIZE = 4096; // Optimal for ESP32 memory

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  this->setup_http_server();
}

bool FTPHTTPProxy::stream_file_from_sd(const std::string& path, httpd_req_t* req) {
  FILE* file = fopen(path.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    return false;
  }

  // Get file size for Content-Length header
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    fclose(file);
    return false;
  }

  char buffer[CHUNK_SIZE];
  size_t bytes_read;
  size_t total_sent = 0;
  
  httpd_resp_set_type(req, "audio/mpeg"); // Adjust MIME type as needed
  httpd_resp_set_hdr(req, "Content-Length", std::to_string(st.st_size).c_str());

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
      ESP_LOGE(TAG, "Send failed");
      break;
    }
    total_sent += bytes_read;
    
    // Yield to prevent watchdog timeout
    if (total_sent % (CHUNK_SIZE * 4) == 0) {
      vTaskDelay(1);
    }
  }

  fclose(file);
  httpd_resp_send_chunk(req, nullptr, 0); // End chunked transfer
  return total_sent == static_cast<size_t>(st.st_size);
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t* req) {
  auto* proxy = (FTPHTTPProxy*)req->user_ctx;
  std::string requested_path = req->uri;

  // Remove leading slash
  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  // Check against allowed paths
  for (const auto& allowed_path : proxy->remote_paths_) {
    if (requested_path == allowed_path) {
      std::string full_path = "/sdcard/" + requested_path; // Adjust path as needed
      
      if (proxy->stream_file_from_sd(full_path, req)) {
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
  config.max_uri_handlers = 8;
  config.stack_size = 8192; // Increased stack for file operations

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




