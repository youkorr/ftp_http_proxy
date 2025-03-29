#include "ftp_http_proxy.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ftp_http_proxy {

static const char *const TAG = "ftp_http_proxy";

void FTPHTTPProxy::setup() {
  ESP_LOGD(TAG, "Setting up FTP to HTTP Proxy");
  setup_ftp_();
  setup_http_();
}

void FTPHTTPProxy::loop() {
  if (server_) {
    server_->handleClient();
  }
}

void FTPHTTPProxy::setup_ftp_() {
  ftp_server_.begin(username_.c_str(), password_.c_str());
  ESP_LOGD(TAG, "FTP server initialized");
}

void FTPHTTPProxy::setup_http_() {
  server_ = new WebServer(local_port_);
  
  server_->on("/", HTTP_GET, [this]() { this->handle_root_(); });
  
  for (const auto &path : remote_paths_) {
    std::string url = "/" + path;
    server_->on(url.c_str(), HTTP_GET, [this, path]() { 
      this->handle_file_(); 
    });
    ESP_LOGD(TAG, "Registered HTTP route: %s", url.c_str());
  }
  
  server_->begin();
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

void FTPHTTPProxy::handle_root_() {
  String message = "FTP to HTTP Proxy\n\nAvailable files:\n";
  
  for (const auto &path : remote_paths_) {
    message += "- " + String(path.c_str()) + "\n";
  }
  
  server_->send(200, "text/plain", message);
}

void FTPHTTPProxy::handle_file_() {
  String path = server_->uri();
  path = path.substring(1); // Remove leading slash
  
  std::string content;
  if (fetch_file_(path.c_str(), content)) {
    server_->send(200, "text/plain", content.c_str());
  } else {
    server_->send(404, "text/plain", "File not found");
  }
}

bool FTPHTTPProxy::fetch_file_(const std::string &path, std::string &content) {
  if (!ftp_client_.connect(ftp_server_.c_str(), 21)) {
    ESP_LOGE(TAG, "Failed to connect to FTP server");
    return false;
  }
  
  if (!ftp_server_.login(ftp_client_, username_.c_str(), password_.c_str())) {
    ESP_LOGE(TAG, "FTP login failed");
    ftp_client_.stop();
    return false;
  }
  
  if (!ftp_server_.download(ftp_client_, path.c_str())) {
    ESP_LOGE(TAG, "Failed to download file: %s", path.c_str());
    ftp_client_.stop();
    return false;
  }
  
  while (ftp_client_.connected()) {
    String line = ftp_client_.readStringUntil('\n');
    content += line.c_str();
  }
  
  ftp_client_.stop();
  ESP_LOGD(TAG, "Successfully fetched file: %s", path.c_str());
  return true;
}

}  // namespace ftp_http_proxy
}  // namespace esphome
