#include "ftp_http_proxy.h"
#include <vector>
#include <string>

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
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
}

void FTPHTTPProxy::setup_http_() {
  server_ = new WebServer(local_port_);
  
  server_->on("/", HTTP_GET, [this]() { this->handle_root_(); });
  
  for (const auto &amp;path : remote_paths_) {
    std::string url = "/" + path;
    server_->on(url.c_str(), HTTP_GET, [this, path]() { 
      this->handle_file_(); 
    });
  }
  
  server_->begin();
}

void FTPHTTPProxy::handle_root_() {
  String message = "FTP to HTTP Proxy\n\nAvailable files:\n";
  
  for (const auto &amp;path : remote_paths_) {
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

bool FTPHTTPProxy::fetch_file_(const std::string &amp;path, std::string &amp;content) {
  if (!ftp_client_.connect(ftp_server_.c_str(), 21)) {
    return false;
  }
  
  if (!ftp_server_.login(ftp_client_, username_.c_str(), password_.c_str())) {
    ftp_client_.stop();
    return false;
  }
  
  if (!ftp_server_.download(ftp_client_, path.c_str())) {
    ftp_client_.stop();
    return false;
  }
  
  while (ftp_client_.connected()) {
    String line = ftp_client_.readStringUntil('\n');
    content += line.c_str();
  }
  
  ftp_client_.stop();
  return true;
}

}  // namespace ftp_http_proxy
}  // namespace esphome

