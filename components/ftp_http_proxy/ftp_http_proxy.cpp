#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "../sd_mmc_card/sd_mmc_card.h"

static const char* TAG = "ftp_proxy";
constexpr size_t STREAM_BUFFER_SIZE = 4096; // Optimisé pour la mémoire ESP32

namespace esphome {
namespace ftp_http_proxy {

FTPHTTPProxy::FTPHTTPProxy() : sock_(-1), server_(nullptr) {}

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP");
  this->setup_http_server();
}

void FTPHTTPProxy::loop() {
  // Maintenance périodique si nécessaire
}

bool FTPHTTPProxy::connect_to_ftp() {
  if (sock_ >= 0) return true; // Connexion déjà établie

  struct sockaddr_in server_addr{};
  struct hostent* ftp_host = gethostbyname(ftp_server_.c_str());
  
  if (!ftp_host || (sock_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ESP_LOGE(TAG, "Échec création socket");
    return false;
  }

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *reinterpret_cast<uint32_t*>(ftp_host->h_addr);

  if (connect(sock_, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Connexion FTP échouée");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentification
  char buffer[256];
  if (cmd_expect("220", 5000) && 
      send_cmd("USER " + username_) && 
      cmd_expect("331", 2000) &&
      send_cmd("PASS " + password_) && 
      cmd_expect("230", 2000) &&
      send_cmd("TYPE I")) {
    return true;
  }

  close(sock_);
  sock_ = -1;
  return false;
}

bool FTPHTTPProxy::stream_file(const std::string& remote_path, httpd_req_t* req) {
  if (!connect_to_ftp()) return false;

  // Activation mode passif
  if (!send_cmd("PASV") || !cmd_expect("227", 2000)) {
    ESP_LOGE(TAG, "Échec mode PASV");
    goto cleanup;
  }

  // Analyse réponse PASV
  int ip[4], port[2];
  char* pasv_start = strchr(recv_buffer_, '(');
  if (!pasv_start || sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", 
                          &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Erreur parsing PASV");
    goto cleanup;
  }

  // Connexion socket données
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec création socket données");
    goto cleanup;
  }

  struct sockaddr_in data_addr{};
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Connexion données échouée");
    close(data_sock);
    goto cleanup;
  }

  // Début transfert
  if (!send_cmd("RETR " + remote_path) || !cmd_expect("150", 2000)) {
    ESP_LOGE(TAG, "Échec commande RETR");
    close(data_sock);
    goto cleanup;
  }

  // Stream direct vers HTTP
  char buffer[STREAM_BUFFER_SIZE];
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send_chunk(req, nullptr, 0); // Début réponse chunkée

  while (true) {
    int bytes_read = recv(data_sock, buffer, sizeof(buffer), 0);
    if (bytes_read <= 0) break;

    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
      ESP_LOGE(TAG, "Erreur envoi HTTP");
      break;
    }
  }

  httpd_resp_send_chunk(req, nullptr, 0); // Fin réponse
  close(data_sock);

  // Vérification fin transfert
  if (!cmd_expect("226", 5000)) {
    ESP_LOGE(TAG, "Transfert incomplet");
    goto cleanup;
  }

cleanup:
  send_cmd("QUIT");
  close(sock_);
  sock_ = -1;
  return true;
}

// Helpers pour la communication FTP
bool FTPHTTPProxy::send_cmd(const std::string& cmd) {
  std::string full_cmd = cmd + "\r\n";
  if (send(sock_, full_cmd.c_str(), full_cmd.size(), 0) < 0) {
    ESP_LOGE(TAG, "Erreur envoi commande: %s", cmd.c_str());
    return false;
  }
  return true;
}

bool FTPHTTPProxy::cmd_expect(const char* code, int timeout_ms) {
  fd_set fds;
  struct timeval tv { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };

  FD_ZERO(&fds);
  FD_SET(sock_, &fds);

  if (select(sock_ + 1, &fds, nullptr, nullptr, &tv) <= 0) {
    ESP_LOGE(TAG, "Timeout commande FTP");
    return false;
  }

  int len = recv(sock_, recv_buffer_, sizeof(recv_buffer_) - 1, 0);
  if (len <= 0) return false;

  recv_buffer_[len] = '\0';
  ESP_LOGD(TAG, "Réponse FTP: %s", recv_buffer_);
  return strstr(recv_buffer_, code) != nullptr;
}

// Gestionnaire HTTP
esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t* req) {
  auto* proxy = static_cast<FTPHTTPProxy*>(req->user_ctx);
  std::string path = req->uri[0] == '/' ? req->uri + 1 : req->uri;

  for (const auto& allowed : proxy->remote_paths_) {
    if (path == allowed) {
      return proxy->stream_file(allowed, req) ? ESP_OK : ESP_FAIL;
    }
  }

  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&server_, &config) == ESP_OK) {
    httpd_register_uri_handler(server_, {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = http_req_handler,
      .user_ctx = this
    });
    ESP_LOGI(TAG, "Serveur HTTP actif sur port %d", local_port_);
  } else {
    ESP_LOGE(TAG, "Échec démarrage serveur HTTP");
  }
}

} // namespace ftp_http_proxy
} // namespace esphome

