#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "../sd_mmc_card/sd_mmc_card.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <mutex>

static const char* TAG = "ftp_proxy";
static const char* SD_MOUNT_POINT = "/sdcard";
static const int FTP_TIMEOUT_MS = 10000;
static const int BUFFER_SIZE = 2048;

namespace esphome {
namespace ftp_http_proxy {

std::mutex ftp_mutex;

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP to HTTP Proxy");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance tasks if needed
}

bool FTPHTTPProxy::connect_to_ftp() {
  std::lock_guard<std::mutex> lock(ftp_mutex);
  
  if (sock_ >= 0) return true;

  struct hostent* ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS resolution failed");
    return false;
  }

  sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed");
    return false;
  }

  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = FTP_TIMEOUT_MS / 1000;
  timeout.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  
  if (setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
      setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
    ESP_LOGW(TAG, "Failed to set socket timeout");
  }

  struct sockaddr_in server_addr {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *reinterpret_cast<uint32_t*>(ftp_host->h_addr);

  if (::connect(sock_, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  std::vector<char> buffer(BUFFER_SIZE);
  int recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "220 ")) {
    ESP_LOGE(TAG, "Invalid FTP welcome message");
    ::close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentication
  snprintf(buffer.data(), buffer.size(), "USER %s\r\n", username_.c_str());
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send USER command");
    disconnect_ftp();
    return false;
  }
  
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "331 ")) {
    ESP_LOGE(TAG, "USER command failed");
    disconnect_ftp();
    return false;
  }

  snprintf(buffer.data(), buffer.size(), "PASS %s\r\n", password_.c_str());
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASS command");
    disconnect_ftp();
    return false;
  }
  
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "230 ")) {
    ESP_LOGE(TAG, "PASS command failed");
    disconnect_ftp();
    return false;
  }

  // Binary mode
  snprintf(buffer.data(), buffer.size(), "TYPE I\r\n");
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send TYPE command");
    disconnect_ftp();
    return false;
  }
  
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0) {
    ESP_LOGE(TAG, "TYPE command failed");
    disconnect_ftp();
    return false;
  }

  return true;
}

void FTPHTTPProxy::disconnect_ftp() {
  std::lock_guard<std::mutex> lock(ftp_mutex);
  
  if (sock_ >= 0) {
    const char* quit_cmd = "QUIT\r\n";
    send(sock_, quit_cmd, strlen(quit_cmd), 0);
    ::close(sock_);
    sock_ = -1;
    ESP_LOGI(TAG, "FTP connection closed");
  }
}

bool FTPHTTPProxy::list_directory(const std::string& path, std::string& content) {
  std::lock_guard<std::mutex> lock(ftp_mutex);
  
  if (!connect_to_ftp()) return false;

  int data_sock = -1;
  auto cleanup = [&]() {
    if (data_sock >= 0) ::close(data_sock);
  };

  std::vector<char> buffer(BUFFER_SIZE);
  snprintf(buffer.data(), buffer.size(), "PASV\r\n");
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASV command");
    cleanup();
    return false;
  }
  
  int recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "227 ")) {
    ESP_LOGE(TAG, "PASV command failed");
    cleanup();
    return false;
  }

  // Parse PASV response
  int ip[4], port[2];
  char* pasv_start = strchr(buffer.data(), '(');
  if (!pasv_start || sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)",
                           &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Invalid PASV response");
    cleanup();
    return false;
  }

  // Create data connection
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    cleanup();
    return false;
  }

  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = FTP_TIMEOUT_MS / 1000;
  timeout.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in data_addr {};
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (::connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connection failed");
    cleanup();
    return false;
  }

  // Start directory listing
  snprintf(buffer.data(), buffer.size(), "LIST %s\r\n", path.c_str());
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send LIST command");
    cleanup();
    return false;
  }
  
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || (!strstr(buffer.data(), "150 ") && !strstr(buffer.data(), "125 "))) {
    ESP_LOGE(TAG, "LIST command failed");
    cleanup();
    return false;
  }

  // Receive directory listing
  content = "<html><head><title>Directory listing</title></head><body>";
  content += "<h1>Directory listing: " + path + "</h1><ul>";
  
  std::vector<char> file_buffer(BUFFER_SIZE);
  int bytes_received;
  std::string raw_listing;
  
  while ((bytes_received = recv(data_sock, file_buffer.data(), file_buffer.size() - 1, 0)) > 0) {
    file_buffer[bytes_received] = '\0';
    raw_listing.append(file_buffer.data());
  }

  // Parse listing and format as HTML
  std::string line;
  size_t pos = 0, next_pos;
  while ((next_pos = raw_listing.find('\n', pos)) != std::string::npos) {
    line = raw_listing.substr(pos, next_pos - pos);
    if (!line.empty()) {
      std::string name;
      bool is_dir = (line[0] == 'd');
      
      size_t name_start = line.find_last_of(" ");
      if (name_start != std::string::npos) {
        name = line.substr(name_start + 1);
        if (!name.empty() && name.back() == '\r') {
          name.pop_back();
        }
        
        if (name != "." && name != "..") {
          if (is_dir) {
            content += "<li><a href=\"" + name + "/\">[DIR] " + name + "</a></li>";
          } else {
            content += "<li><a href=\"" + name + "\">" + name + "</a></li>";
          }
        }
      }
    }
    pos = next_pos + 1;
  }
  
  content += "</ul></body></html>";
  cleanup();

  // Verify transfer completion
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "226 ")) {
    ESP_LOGW(TAG, "Directory listing may be incomplete");
  }
  
  return true;
}

bool FTPHTTPProxy::download_file(const std::string& remote_path, const std::string& local_path) {
  std::lock_guard<std::mutex> lock(ftp_mutex);
  
  if (!connect_to_ftp()) return false;

  ESP_LOGI(TAG, "Downloading %s to %s", remote_path.c_str(), local_path.c_str());

  // Ensure parent directories exist
  size_t last_slash = local_path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir_path = local_path.substr(0, last_slash);
    if (!ensure_directory(dir_path)) {
      ESP_LOGE(TAG, "Failed to create directory structure for %s", dir_path.c_str());
      return false;
    }
  }

  int data_sock = -1;
  int fd = -1;
  auto cleanup = [&]() {
    if (data_sock >= 0) ::close(data_sock);
    if (fd >= 0) ::close(fd);
  };

  std::vector<char> buffer(BUFFER_SIZE);
  snprintf(buffer.data(), buffer.size(), "PASV\r\n");
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send PASV command");
    cleanup();
    return false;
  }
  
  int recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "227 ")) {
    ESP_LOGE(TAG, "PASV command failed");
    cleanup();
    return false;
  }

  // Parse PASV response
  int ip[4], port[2];
  char* pasv_start = strchr(buffer.data(), '(');
  if (!pasv_start || sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)",
                           &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Invalid PASV response");
    cleanup();
    return false;
  }

  // Create data connection
  data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    cleanup();
    return false;
  }

  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = FTP_TIMEOUT_MS / 1000;
  timeout.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in data_addr {};
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (::connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connection failed");
    cleanup();
    return false;
  }

  // Start file transfer
  snprintf(buffer.data(), buffer.size(), "RETR %s\r\n", remote_path.c_str());
  if (send(sock_, buffer.data(), strlen(buffer.data()), 0) < 0) {
    ESP_LOGE(TAG, "Failed to send RETR command");
    cleanup();
    return false;
  }
  
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || (!strstr(buffer.data(), "150 ") && !strstr(buffer.data(), "125 "))) {
    ESP_LOGE(TAG, "RETR command failed");
    cleanup();
    return false;
  }

  // Open local file for writing
  fd = open(local_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ESP_LOGE(TAG, "Failed to open local file: %s (errno: %d)", local_path.c_str(), errno);
    cleanup();
    return false;
  }

  // Receive file content and write to disk
  std::vector<char> file_buffer(BUFFER_SIZE);
  int bytes_received;
  size_t total_bytes = 0;
  
  while ((bytes_received = recv(data_sock, file_buffer.data(), file_buffer.size(), 0)) > 0) {
    if (write(fd, file_buffer.data(), bytes_received) != bytes_received) {
      ESP_LOGE(TAG, "Failed to write to local file");
      cleanup();
      return false;
    }
    total_bytes += bytes_received;
  }

  ESP_LOGI(TAG, "Downloaded %zu bytes to %s", total_bytes, local_path.c_str());
  cleanup();

  // Verify transfer completion
  recv_len = recv(sock_, buffer.data(), buffer.size() - 1, 0);
  if (recv_len <= 0 || !strstr(buffer.data(), "226 ")) {
    ESP_LOGW(TAG, "Transfer may be incomplete");
  }

  return true;
}

bool FTPHTTPProxy::ensure_directory(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return true;
    }
    ESP_LOGE(TAG, "%s exists but is not a directory", path.c_str());
    return false;
  }
  
  // Create parent directories recursively
  size_t pos = 0;
  std::string current_path;
  
  if (path.size() > 0 && path[0] == '/') {
    pos = 1;
  }
  
  while ((pos = path.find('/', pos)) != std::string::npos) {
    current_path = path.substr(0, pos);
    if (!current_path.empty()) {
      if (stat(current_path.c_str(), &st) != 0) {
        if (mkdir(current_path.c_str(), 0755) != 0 && errno != EEXIST) {
          ESP_LOGE(TAG, "Failed to create directory: %s (errno: %d)", current_path.c_str(), errno);
          return false;
        }
      } else if (!S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "%s exists but is not a directory", current_path.c_str());
        return false;
      }
    }
    pos++;
  }
  
  if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    ESP_LOGE(TAG, "Failed to create directory: %s (errno: %d)", path.c_str(), errno);
    return false;
  }
  
  return true;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t* req) {
  auto* proxy = (FTPHTTPProxy*)req->user_ctx;
  std::string requested_path = req->uri;

  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }
  
  if (requested_path.empty()) {
    requested_path = "/";
  }
  
  bool is_directory = (!requested_path.empty() && requested_path.back() == '/');
  
  ESP_LOGI(TAG, "Requested path: %s (is_directory: %d)", requested_path.c_str(), is_directory);
  
  if (is_directory) {
    std::string content;
    if (proxy->list_directory(requested_path, content)) {
      httpd_resp_set_type(req, "text/html");
      httpd_resp_send(req, content.c_str(), content.size());
      return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to list directory");
    return ESP_FAIL;
  } else {
    std::string local_path = std::string(SD_MOUNT_POINT) + "/cache/" + requested_path;
    
    struct stat st;
    if (stat(local_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      ESP_LOGI(TAG, "Serving cached file: %s", local_path.c_str());
      
      const char* content_type = "application/octet-stream";
      if (local_path.find(".html") != std::string::npos || local_path.find(".htm") != std::string::npos) {
        content_type = "text/html";
      } else if (local_path.find(".txt") != std::string::npos) {
        content_type = "text/plain";
      } else if (local_path.find(".jpg") != std::string::npos || local_path.find(".jpeg") != std::string::npos) {
        content_type = "image/jpeg";
      } else if (local_path.find(".png") != std::string::npos) {
        content_type = "image/png";
      } else if (local_path.find(".css") != std::string::npos) {
        content_type = "text/css";
      } else if (local_path.find(".js") != std::string::npos) {
        content_type = "application/javascript";
      } else if (local_path.find(".pdf") != std::string::npos) {
        content_type = "application/pdf";
      }
      
      httpd_resp_set_type(req, content_type);
      
      int fd = open(local_path.c_str(), O_RDONLY);
      if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open cached file: %s", local_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read cached file");
        return ESP_FAIL;
      }
      
      std::vector<char> buffer(BUFFER_SIZE);
      ssize_t bytes_read;
      while ((bytes_read = read(fd, buffer.data(), buffer.size())) > 0) {
        httpd_resp_send_chunk(req, buffer.data(), bytes_read);
      }
      
      close(fd);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_OK;
    }
    
    ESP_LOGI(TAG, "File not in cache, downloading from FTP: %s", requested_path.c_str());
    
    proxy->ensure_directory(std::string(SD_MOUNT_POINT) + "/cache");
    
    if (proxy->download_file(requested_path, local_path)) {
      ESP_LOGI(TAG, "Download successful, serving file: %s", local_path.c_str());
      
      const char* content_type = "application/octet-stream";
      if (local_path.find(".html") != std::string::npos || local_path.find(".htm") != std::string::npos) {
        content_type = "text/html";
      } else if (local_path.find(".txt") != std::string::npos) {
        content_type = "text/plain";
      } else if (local_path.find(".jpg") != std::string::npos || local_path.find(".jpeg") != std::string::npos) {
        content_type = "image/jpeg";
      } else if (local_path.find(".png") != std::string::npos) {
        content_type = "image/png";
      } else if (local_path.find(".css") != std::string::npos) {
        content_type = "text/css";
      } else if (local_path.find(".js") != std::string::npos) {
        content_type = "application/javascript";
      } else if (local_path.find(".pdf") != std::string::npos) {
        content_type = "application/pdf";
      }
      
      httpd_resp_set_type(req, content_type);
      
      int fd = open(local_path.c_str(), O_RDONLY);
      if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open downloaded file: %s", local_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read downloaded file");
        return ESP_FAIL;
      }
      
      std::vector<char> buffer(BUFFER_SIZE);
      ssize_t bytes_read;
      while ((bytes_read = read(fd, buffer.data(), buffer.size())) > 0) {
        httpd_resp_send_chunk(req, buffer.data(), bytes_read);
      }
      
      close(fd);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_OK;
    }
    
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found or download failed");
    return ESP_FAIL;
  }
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.stack_size = 16384; // Increased stack size
  config.max_uri_handlers = 16;
  config.max_open_sockets = 13;

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
  
  ensure_directory(std::string(SD_MOUNT_POINT) + "/cache");
}

}  // namespace ftp_http_proxy
}  // namespace esphome




