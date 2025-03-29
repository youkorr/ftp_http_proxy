#include "ftp_http_proxy.h"
#include "esphome/core/log.h"
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESP32FtpServer.h>

namespace esphome {
namespace ftp_http_proxy {

static const char *const TAG = "ftp_http_proxy";
static WebServer *server = nullptr;
static FtpServer *ftp_server = nullptr;

void FTPHTTPProxy::setup() {
  ESP_LOGD(TAG, "Setting up FTP to HTTP Proxy");
  
  // Initialize FTP server
  ftp_server = new FtpServer();
  ftp_server->begin(username_.c_str(), password_.c_str());
  ESP_LOGD(TAG, "FTP server initialized");

  // Initialize HTTP server
  server = new WebServer(local_port_);
  
  server->on("/", HTTP_GET, [this]() { this->handle_root_(); });
  
  for (const auto &path : remote_paths_) {
    std::string url = "/" + path;
    server->on(url.c_str(), HTTP_GET, [this, path]() {
      this->handle_file_(path);
    });
    ESP_LOGD(TAG, "Registered HTTP route: %s", url.c_str());
  }
  
  server->begin();
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

void FTPHTTPProxy::loop() {
  if (server) {
    server->handleClient();
  }
}

void FTPHTTPProxy::handle_root_() {
  String message = "FTP to HTTP Proxy\n\nAvailable files:\n";
  
  for (const auto &path : remote_paths_) {
    message += "- " + String(path.c_str()) + "\n";
  }
  
  server->send(200, "text/plain", message);
}

void FTPHTTPProxy::handle_file_(const std::string &path) {
  std::string content;
  WiFiClient ftp_client;
  
  if (!ftp_client.connect(ftp_server_.c_str(), 21)) {
    ESP_LOGE(TAG, "Failed to connect to FTP server");
    server->send(500, "text/plain", "FTP connection failed");
    return;
  }
  
  if (!ftp_server->login(ftp_client, username_.c_str(), password_.c_str())) {
    ESP_LOGE(TAG, "FTP login failed");
    server->send(401, "text/plain", "FTP authentication failed");
    ftp_client.stop();
    return;
  }
  
  if (!ftp_server->download(ftp_client, path.c_str())) {
    ESP_LOGE(TAG, "Failed to download file: %s", path.c_str());
    server->send(404, "text/plain", "File not found on FTP server");
    ftp_client.stop();
    return;
  }
  
  String content;
  while (ftp_client.connected()) {
    content += ftp_client.readString();
  }
  
  ftp_client.stop();
  server->send(200, "text/plain", content);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
