#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

// static const char *TAG = "HTTP_TCP";
static const char *HOST = "httpbin.org";
static const char *PORT = "80"; // HTTPS 需要 mbedtls，这里演示纯 HTTP

bool fetch_httpbin_via_tcp(void) {
  char rx_buffer[512];
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *res;

  // 1. 获取地址信息
  if (getaddrinfo(HOST, PORT, &hints, &res) != 0)
    return false;

  // 2. 创建 Socket
  int sock = socket(res->ai_family, res->ai_socktype, 0);
  if (sock < 0) {
    freeaddrinfo(res);
    return false;
  }

  // 3. 连接
  if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
    close(sock);
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);
  printf("-------------------------------------------\n\n");
  // 4. 发送 HTTP GET 请求报文
  const char *req = "GET /get HTTP/1.1\r\n"
                    "Host: httpbin.org\r\n"
                    "Accept: application/json\r\n"
                    "Connection: close\r\n\r\n";
  size_t req_len = strlen(req);
  size_t sent = 0;
  while (sent < req_len) {
    int ret = write(sock, req + sent, req_len - sent);
    if (ret <= 0) {
      close(sock);
      return false;
    }
    sent += (size_t)ret;
  }

  // char json_body[] = "{\"key\":\"value\"}";
  // char req[512];
  // snprintf(req, sizeof(req),
  //          "POST /post HTTP/1.1\r\n"
  //          "Host: httpbin.org\r\n"
  //          "Content-Type: application/json\r\n"
  //          "Content-Length: %d\r\n"
  //          "Connection: close\r\n\r\n"
  //          "%s", (int)strlen(json_body), json_body);
  //
  // write(sock, req, strlen(req));

  // 5. 接收并打印 Payload
  bool header_passed = false;
  while (1) {
    int len = read(sock, rx_buffer, sizeof(rx_buffer) - 1);
    if (len <= 0)
      break;
    rx_buffer[len] = 0;

    // 简单的 HTTP 头部过滤 (跳过 \r\n\r\n 之前的内容)
    if (!header_passed) {
      char *body = strstr(rx_buffer, "\r\n\r\n");
      if (body) {
        printf("%s", body + 4);
        header_passed = true;
      }
    } else {
      printf("%s", rx_buffer);
    }
  }
  printf("-------------------------------------------\n\n");
  close(sock);
  return true;
}
