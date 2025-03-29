#include "ftp_http_proxy.h"
#include <esp_log.h>
#include <lwip/netdb.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include "../sd_mmc_card/sd_mmc_card.h"

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_http_proxy";

FTPHTTPProxy::FTPHTTPProxy() : ftp_port_(21), ftp_sock_(-1) {}

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
        close(ftp_sock_);
        ftp_sock_ = -1;
        return false;
    }

    return true;
}

bool FTPHTTPProxy::send_ftp_command(const std::string &cmd, std::string &response) {
    if (ftp_sock_ < 0) return false;

    if (send(ftp_sock_, cmd.c_str(), cmd.size(), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send command: %s", cmd.c_str());
        return false;
    }

    char buffer[512];
    int len = recv(ftp_sock_, buffer, sizeof(buffer) - 1, 0);
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to receive response");
        return false;
    }
    buffer[len] = '\0';
    response = buffer;
    return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, std::string &content) {
    if (!connect_to_ftp()) return false;
    
    std::string response;
    if (!send_ftp_command("USER anonymous\r\n", response) || response.find("331") == std::string::npos) return false;
    if (!send_ftp_command("PASS anonymous@\r\n", response) || response.find("230") == std::string::npos) return false;
    if (!send_ftp_command("TYPE I\r\n", response) || response.find("200") == std::string::npos) return false;
    if (!send_ftp_command("RETR " + remote_path + "\r\n", response) || response.find("150") == std::string::npos) return false;
    
    char buffer[1024];
    int len;
    while ((len = recv(ftp_sock_, buffer, sizeof(buffer), 0)) > 0) {
        content.append(buffer, len);
    }
    
    close(ftp_sock_);
    ftp_sock_ = -1;
    return true;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
    std::string sanitized_path = "/correct/path"; 
    std::string file_content;
    FTPHTTPProxy *proxy = static_cast<FTPHTTPProxy *>(req->user_ctx);
    
    if (!proxy->download_file(sanitized_path, file_content)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_send(req, file_content.c_str(), file_content.size());
    return ESP_OK;
}

}  // namespace ftp_http_proxy
}  // namespace esphome


