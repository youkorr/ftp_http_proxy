#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <cstring>
#include <vector>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

bool FTPHTTPProxy::download_file(const std::string &remote_path, httpd_req_t *req) {
  if (!connect_to_ftp()) return false;

  char buffer[512];
  snprintf(buffer, sizeof(buffer), "PASV\r\n");
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "PASV failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  int ip[4], port[2];
  char *ptr = strchr(buffer, '(');
  if (!ptr || sscanf(ptr, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "PASV parse error");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  int data_sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket failed");
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(port[0] * 256 + port[1]);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  // Set socket timeout
  struct timeval tv;
  tv.tv_sec = 10;  // 10 second timeout
  tv.tv_usec = 0;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (lwip_connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connect failed");
    lwip_close(data_sock);
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  if (lwip_send(sock_, buffer, strlen(buffer), 0) < 0 || 
      lwip_recv(sock_, buffer, sizeof(buffer), 0) <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "RETR failed");
    lwip_close(data_sock);
    lwip_close(sock_);
    sock_ = -1;
    return false;
  }

  // Detect file type and set appropriate content type
  const char* content_type = "application/octet-stream";
  std::string file_ext = remote_path.substr(remote_path.find_last_of('.') + 1);
  if (file_ext == "txt") content_type = "text/plain";
  else if (file_ext == "html") content_type = "text/html";
  else if (file_ext == "json") content_type = "application/json";
  else if (file_ext == "xml") content_type = "application/xml";

  httpd_resp_set_type(req, content_type);
  httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

  // Use a fixed-size buffer on the heap to avoid stack overflow
  const size_t CHUNK_SIZE = 2048;  // Reduced chunk size for better memory management
  std::vector<char> chunk(CHUNK_SIZE);
  size_t total_transferred = 0;
  int consecutive_zero_reads = 0;
  bool transfer_complete = false;

  while (!transfer_complete) {
    // Check for stack overflow or memory issues
    if (total_transferred > 1024 * 100) {  // Limit to 100 KB as safety check
      ESP_LOGW(TAG, "Transfer size limit reached");
      break;
    }

    // Perform receive with error checking
    ssize_t len = lwip_recv(data_sock, chunk.data(), chunk.size(), 0);
    
    if (len > 0) {
      // Successfully read data
      consecutive_zero_reads = 0;
      esp_err_t send_result = httpd_resp_send_chunk(req, chunk.data(), len);
      
      if (send_result != ESP_OK) {
        ESP_LOGE(TAG, "Chunk send failed: %d", send_result);
        break;
      }
      
      total_transferred += len;
    } else if (len == 0) {
      // Potential end of transfer
      consecutive_zero_reads++;
      if (consecutive_zero_reads >= 3) {
        transfer_complete = true;
      }
      
      // Short delay to prevent tight loop
      vTaskDelay(pdMS_TO_TICKS(50));
    } else {
      // Error condition
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGE(TAG, "Receive error: %d", errno);
        break;
      }
    }

    // Yield to other tasks
    taskYIELD();
  }

  // Finalize transfer
  httpd_resp_send_chunk(req, NULL, 0);
  
  // Close data socket
  lwip_close(data_sock);

  // Cleanup FTP connection
  lwip_recv(sock_, buffer, sizeof(buffer), 0); // Wait for 226
  snprintf(buffer, sizeof(buffer), "QUIT\r\n");
  lwip_send(sock_, buffer, strlen(buffer), 0);
  lwip_close(sock_);
  sock_ = -1;

  ESP_LOGI(TAG, "Transferred %d bytes", total_transferred);
  return total_transferred > 0;
}

}  // namespace ftp_http_proxy
}  // namespace esphome

