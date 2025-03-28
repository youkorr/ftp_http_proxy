#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_proxy";

// Static utility method for removing quotes
std::string clean_allowed_path = FTPHTTPProxy::remove_quotes(allowed_path);
    if (str.length() >= 2 &&
        str.front() == '"' &&
        str.back() == '"') {
        return str.substr(1, str.length() - 2);
    }
    return str;
}

void FTPHTTPProxy::setup() {
    // Validation des paramètres de configuration
    if (ftp_server_.empty()) {
        ESP_LOGE(TAG, "FTP server address is required");
        return;
    }
    if (username_.empty() || password_.empty()) {
        ESP_LOGE(TAG, "FTP authentication credentials are required");
        return;
    }
    if (remote_paths_.empty()) {
        ESP_LOGE(TAG, "At least one remote path must be configured");
        return;
    }
    // Configuration du serveur HTTP
    ESP_LOGI(TAG, "Initializing FTP HTTP Proxy");
    ESP_LOGI(TAG, "FTP Server: %s", ftp_server_.c_str());
    ESP_LOGI(TAG, "Local HTTP Port: %d", local_port_);
    // Affichage des chemins autorisés avec gestion des guillemets
    ESP_LOGI(TAG, "Allowed Remote Paths:");
    for (const auto& path : remote_paths_) {
        std::string clean_path = remove_quotes(path);  // Correction ici
        ESP_LOGI(TAG, "- %s", clean_path.c_str());
    }
    // Configuration du serveur HTTP sécurisé
    setup_http_server();
}

void FTPHTTPProxy::loop() {
    // Optionnel : Ajouter des vérifications périodiques ou maintenance
    // Par exemple, vérifier la connectivité réseau
}

bool FTPHTTPProxy::connect_to_ftp() {
    // Gérer la réinitialisation du socket s'il est déjà connecté
    if (sock_ >= 0) {
        lwip_close(sock_);
        sock_ = -1;
    }
    // Création du socket avec gestion des erreurs améliorée
    sock_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "Socket creation failed: %s", strerror(errno));
        return false;
    }
    // Résolution du nom d'hôte avec gestion d'erreur
    struct hostent *host = gethostbyname(ftp_server_.c_str());
    if (!host) {
        ESP_LOGE(TAG, "Hostname resolution failed for %s", ftp_server_.c_str());
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    // Configuration de l'adresse du serveur
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ftp_port_);
    memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);
    // Configuration du timeout avec gestion d'erreur
    struct timeval tv;
    tv.tv_sec = 10;  // Timeout de 10 secondes
    tv.tv_usec = 0;
    if (setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "Failed to set socket timeout");
        // Continue even if timeout setting fails
    }
    // Connexion au serveur avec gestion d'erreur détaillée
    if (lwip_connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Connection to FTP server failed: %s", strerror(errno));
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    // Authentification FTP avec gestion des erreurs
    if (!authenticate_ftp()) {
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    return true;
}

bool FTPHTTPProxy::authenticate_ftp() {
    char buffer[512];
    ssize_t recv_len;
    // Réception du message de bienvenue
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No welcome message from FTP server");
        return false;
    }
    buffer[recv_len] = '\0';
    ESP_LOGI(TAG, "FTP Server Welcome: %s", buffer);
    // Envoi du nom d'utilisateur
    snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send username");
        return false;
    }
    // Réception de la réponse à la commande USER
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No response to USER command");
        return false;
    }
    buffer[recv_len] = '\0';
    ESP_LOGI(TAG, "USER Response: %s", buffer);
    // Envoi du mot de passe
    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send password");
        return false;
    }
    // Réception de la réponse à la commande PASS
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No response to PASS command");
        return false;
    }
    buffer[recv_len] = '\0';
    ESP_LOGI(TAG, "PASS Response: %s", buffer);
    // Vérifier si l'authentification a réussi (code 230)
    if (strstr(buffer, "230") == nullptr) {
        ESP_LOGE(TAG, "Authentication failed: %s", buffer);
        return false;
    }
    return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
    // Vérification des paramètres
    if (remote_path.empty()) {
        ESP_LOGE(TAG, "Empty remote path");
        return false;
    }
    // Connexion au serveur FTP
    if (!connect_to_ftp()) {
        ESP_LOGE(TAG, "Failed to connect to FTP server for file: %s", remote_path.c_str());
        return false;
    }
    // Commande PASV pour le mode passif avec gestion d'erreur
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "PASV\r\n");
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "PASV command failed");
        return false;
    }
    // Réception de la réponse PASV
    ssize_t recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No PASV response");
        return false;
    }
    buffer[recv_len] = '\0';
    ESP_LOGI(TAG, "PASV Response: %s", buffer);
    // Parsing de l'adresse IP et du port avec validation
    int ip[4], port[2];
    char *ptr = strchr(buffer, '(');
    if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", 
                       &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
        ESP_LOGE(TAG, "Failed to parse PASV response");
        return false;
    }
    // Création du socket de données avec gestion d'erreur
    int data_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (data_sock < 0) {
        ESP_LOGE(TAG, "Failed to create data socket");
        return false;
    }
    // Configuration de l'adresse de données
    struct sockaddr_in data_addr;
    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(port[0] * 256 + port[1]);
    data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);
    // Connexion au socket de données
    if (lwip_connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to data socket");
        lwip_close(data_sock);
        return false;
    }
    // Commande RETR pour récupérer le fichier
    snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "RETR command failed for file: %s", remote_path.c_str());
        lwip_close(data_sock);
        return false;
    }
    // Réception de la réponse RETR
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0 || strstr(buffer, "150") == nullptr) {
        ESP_LOGE(TAG, "File transfer not started for: %s", remote_path.c_str());
        lwip_close(data_sock);
        return false;
    }
    // Configuration du type MIME
    std::string file_ext = get_file_extension(remote_path);
    const char* mime_type = get_mime_type(file_ext);
    httpd_resp_set_type(req, mime_type);
    // Streaming du fichier avec gestion des erreurs
    std::vector<char> chunk(2048);
    size_t total_transferred = 0;
    while (true) {
        ssize_t len = lwip_recv(data_sock, chunk.data(), chunk.size(), 0);
        if (len > 0) {
            esp_err_t send_result = httpd_resp_send_chunk(req, chunk.data(), len);
            if (send_result != ESP_OK) {
                ESP_LOGE(TAG, "Chunk send failed");
                break;
            }
            total_transferred += len;
        } else if (len == 0) {
            break;  // Fin du transfert
        } else {
            ESP_LOGE(TAG, "Receive error");
            break;
        }
    }
    // Finalisation du transfert
    httpd_resp_send_chunk(req, NULL, 0);
    // Fermeture des sockets
    lwip_close(data_sock);
    // Commande QUIT
    snprintf(buffer, sizeof(buffer), "QUIT\r\n");
    lwip_send(sock_, buffer, strlen(buffer), 0);
    lwip_close(sock_);
    sock_ = -1;
    ESP_LOGI(TAG, "Transferred %d bytes for file: %s", total_transferred, remote_path.c_str());
    return total_transferred > 0;
}

std::string FTPHTTPProxy::get_file_extension(const std::string& filename) {
    size_t pos = filename.find_last_of('.');
    return (pos != std::string::npos) ? filename.substr(pos + 1) : "";
}

const char* FTPHTTPProxy::get_mime_type(const std::string& extension) {
    if (extension == "png") return "image/png";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "gif") return "image/gif";
    if (extension == "pdf") return "application/pdf";
    if (extension == "txt") return "text/plain";
    if (extension == "mp3") return "audio/mpeg";
    if (extension == "mp4") return "video/mp4";
    return "application/octet-stream";  // Type générique par défaut
}

void FTPHTTPProxy::setup_http_server() {
    // Configuration du serveur HTTP
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = local_port_;
    config.max_uri_handlers = 10;  // Augmenter si nécessaire
    config.stack_size = 8192;  // Augmenter la taille de la pile si besoin
    // Démarrage du serveur avec gestion d'erreur
    esp_err_t start_result = httpd_start(&server_, &config);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server. Error: %s", esp_err_to_name(start_result));
        return;
    }
    // Configuration du gestionnaire de requêtes
    httpd_uri_t uri_handler = {
        .uri       = "/*",  // Écouter toutes les URIs
        .method    = HTTP_GET,
        .handler   = http_req_handler,
        .user_ctx  = this
    };
    esp_err_t handler_result = httpd_register_uri_handler(server_, &uri_handler);
    if (handler_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler. Error: %s", esp_err_to_name(handler_result));
        httpd_stop(server_);
    }
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
    // Récupération du contexte
    FTPHTTPProxy* proxy = static_cast<FTPHTTPProxy*>(req->user_ctx);
    // Journalisation de l'URI complète
    ESP_LOGI(TAG, "Received Request URI: %s", req->uri);
    // Extraction et nettoyage du chemin de fichier
    std::string sanitized_path = req->uri;
    // Supprimer le '/' initial si présent
    if (!sanitized_path.empty() && sanitized_path[0] == '/') {
        sanitized_path = sanitized_path.substr(1);
    }
    // Vérification du chemin
    if (sanitized_path.empty()) {
        ESP_LOGE(TAG, "Empty file path requested");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file path");
        return ESP_FAIL;
    }
    // Vérification des chemins autorisés
    bool path_allowed = false;
    for (const auto& allowed_path : proxy->remote_paths_) {
        std::string clean_allowed_path = remove_quotes(allowed_path);  // Correction ici
        // Vérifier si le chemin demandé correspond ou est un sous-chemin
        if (sanitized_path.find(clean_allowed_path) == 0) {
            path_allowed = true;
            break;
        }
    }
    // Si le chemin n'est pas autorisé, renvoyer une erreur
    if (!path_allowed) {
        ESP_LOGE(TAG, "Unauthorized file path: %s", sanitized_path.c_str());
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "File access not permitted");
        return ESP_FAIL;
    }
    // Tentative de téléchargement du fichier
    if (!proxy->download_file(sanitized_path, req)) {
        ESP_LOGE(TAG, "Download failed for file: %s", sanitized_path.c_str());
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File download failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

}  // namespace ftp_http_proxy
}  // namespace esphome





