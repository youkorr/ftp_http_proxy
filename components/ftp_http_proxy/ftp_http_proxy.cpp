#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace esphome {
namespace ftp_http_proxy {

static const char *TAG = "ftp_proxy";

void FTPHTTPProxy::setup() {
    // Configuration initiale du composant
    ESP_LOGI(TAG, "Setting up FTP HTTP Proxy");
    
    // Vérifier que les paramètres essentiels sont définis
    if (ftp_server_.empty() || username_.empty() || password_.empty()) {
        ESP_LOGE(TAG, "FTP server configuration incomplete");
        return;
    }

    // Log des chemins autorisés
    ESP_LOGI(TAG, "Allowed remote paths:");
    for (const auto& path : remote_paths_) {
        ESP_LOGI(TAG, "- %s", path.c_str());
    }

    // Configurer le serveur HTTP
    setup_http_server();
}

void FTPHTTPProxy::loop() {
    // Logique périodique si nécessaire
    // Par exemple, vérifier la connexion ou gérer des tâches en arrière-plan
}

bool FTPHTTPProxy::connect_to_ftp() {
    // Création du socket
    sock_ = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    // Résolution du nom d'hôte
    struct hostent *host = gethostbyname(ftp_server_.c_str());
    if (!host) {
        ESP_LOGE(TAG, "Failed to resolve hostname");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Configuration de l'adresse du serveur
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ftp_port_);  // Utiliser le port FTP configuré
    memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);

    // Configuration du timeout
    struct timeval tv;
    tv.tv_sec = 10;  // 10 secondes de timeout
    tv.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Connexion au serveur
    if (lwip_connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to FTP server");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Réception du message de bienvenue
    char buffer[512];
    ssize_t recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No response from FTP server");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    buffer[recv_len] = '\0';

    // Authentification
    // Envoi du nom d'utilisateur
    snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send username");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Réception de la réponse à la commande USER
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No response to USER command");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    buffer[recv_len] = '\0';

    // Envoi du mot de passe
    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send password");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Réception de la réponse à la commande PASS
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No response to PASS command");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    buffer[recv_len] = '\0';

    // Vérifier si l'authentification a réussi (code 230)
    if (strstr(buffer, "230") == nullptr) {
        ESP_LOGE(TAG, "Authentication failed");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    return true;
}

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
    // Connexion au serveur FTP
    if (!connect_to_ftp()) {
        ESP_LOGE(TAG, "Failed to connect to FTP");
        return false;
    }

    // Commande PASV pour le mode passif
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "PASV\r\n");
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "PASV command failed");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Réception de la réponse PASV
    ssize_t recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "No PASV response");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    buffer[recv_len] = '\0';

    // Parsing de l'adresse IP et du port
    int ip[4], port[2];
    char *ptr = strchr(buffer, '(');
    if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", 
                       &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
        ESP_LOGE(TAG, "Failed to parse PASV response");
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Création du socket de données
    int data_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (data_sock < 0) {
        ESP_LOGE(TAG, "Failed to create data socket");
        lwip_close(sock_);
        sock_ = -1;
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
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Commande RETR pour récupérer le fichier
    snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
    if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0) {
        ESP_LOGE(TAG, "RETR command failed");
        lwip_close(data_sock);
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Réception de la réponse RETR
    recv_len = lwip_recv(sock_, buffer, sizeof(buffer) - 1, 0);
    if (recv_len <= 0 || strstr(buffer, "150") == nullptr) {
        ESP_LOGE(TAG, "File transfer not started");
        lwip_close(data_sock);
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }

    // Streaming du fichier
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

    ESP_LOGI(TAG, "Transferred %d bytes", total_transferred);
    return total_transferred > 0;
}

void FTPHTTPProxy::setup_http_server() {
    // Configuration du serveur HTTP
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = local_port_; // Utiliser le port HTTP configuré
    config.max_uri_handlers = 10;  // Augmenter si nécessaire
    config.stack_size = 8192;  // Augmenter la taille de la pile si besoin

    // Démarrage du serveur
    esp_err_t start_result = httpd_start(&server_, &config);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server. Error: %s", esp_err_to_name(start_result));
        return;
    }

    // Configuration du gestionnaire de requêtes
    httpd_uri_t uri_handler = {
        .uri       = "/download/*",
        .method    = HTTP_GET,
        .handler   = http_req_handler,
        .user_ctx  = this
    };
    
    esp_err_t handler_result = httpd_register_uri_handler(server_, &uri_handler);
    if (handler_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler. Error: %s", esp_err_to_name(handler_result));
    }
}

// Fonction utilitaire pour supprimer les guillemets
std::string remove_quotes(const std::string& str) {
    if (str.length() >= 2 && 
        str.front() == '"' && 
        str.back() == '"') {
        return str.substr(1, str.length() - 2);
    }
    return str;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
    // Log the full URI for debugging
    ESP_LOGI(TAG, "Received URI: %s", req->uri);
    
    // Récupération du contexte
    FTPHTTPProxy* proxy = static_cast<FTPHTTPProxy*>(req->user_ctx);
    
    // More robust path extraction
    const char* download_prefix = "/download/";
    size_t prefix_len = strlen(download_prefix);
    
    // Check if URI starts with the expected prefix
    if (strncmp(req->uri, download_prefix, prefix_len) != 0) {
        ESP_LOGE(TAG, "URI does not start with %s. Actual URI: %s", download_prefix, req->uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid URI format");
        return ESP_FAIL;
    }

    // Extraction du chemin de fichier à partir de l'URI
    char file_path[256];
    strlcpy(file_path, req->uri + prefix_len, sizeof(file_path));

    // Log the extracted file path
    ESP_LOGI(TAG, "Extracted file path: %s", file_path);

    // Sanitize the path (remove quotes if present)
    std::string sanitized_path = remove_quotes(file_path);

    // Log the sanitized path
    ESP_LOGI(TAG, "Sanitized file path: %s", sanitized_path.c_str());

    // Log the list of allowed remote paths
    ESP_LOGI(TAG, "Allowed remote paths:");
    for (const auto& path : proxy->remote_paths_) {
        ESP_LOGI(TAG, "- %s", path.c_str());
    }

    // Vérifier si le fichier est dans la liste des chemins autorisés
    bool path_allowed = false;
    for (const auto& allowed_path : proxy->remote_paths_) {
        // Remove quotes from the allowed path for comparison
        std::string clean_allowed_path = remove_quotes(allowed_path);
        
        if (clean_allowed_path == sanitized_path) {
            path_allowed = true;
            break;
        }
    }

    // Si le chemin n'est pas autorisé, renvoyer une erreur
    if (!path_allowed) {
        ESP_LOGE(TAG, "File path not found in allowed paths: %s", sanitized_path.c_str());
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File not allowed");
        return ESP_FAIL;
    }

    // Tentative de téléchargement du fichier
    if (!proxy->download_file(sanitized_path, req)) {
        ESP_LOGE(TAG, "Download failed for file: %s", sanitized_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Download failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

}  // namespace ftp_http_proxy
}  // namespace esphome



