#include "ftp_http_proxy.h"
#include <esp_log.h>
#include <lwip/netdb.h> // Utilisation de getaddrinfo() à la place de gethostbyname()

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_http_proxy";

bool FTPHTTPProxy::connect_to_ftp() {
    struct addrinfo hints = {};
    struct addrinfo *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(ftp_server_.c_str(), nullptr, &hints, &res);
    if (err != 0 || res == nullptr) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", ftp_server_.c_str());
        return false;
    }

    struct sockaddr_in server_addr = *(struct sockaddr_in *) res->ai_addr;
    server_addr.sin_port = htons(ftp_port_);
    freeaddrinfo(res);

    ftp_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (ftp_sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    if (connect(ftp_sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to connect to FTP server");
        return false;
    }

    return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, std::string &content) {
    // Logique pour télécharger le fichier et stocker dans 'content'
    return true;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
    std::string sanitized_path = "/correct/path"; // Traitement du chemin
    std::string file_content;
    
    if (!download_file(sanitized_path, file_content)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_send(req, file_content.c_str(), file_content.size());
    return ESP_OK;
}

}  // namespace ftp_http_proxy
}  // namespace esphome


