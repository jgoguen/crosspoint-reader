#pragma once

#include <Stream.h>
#include <esp_http_client.h>

#include <cstddef>
#include <cstdint>

class HttpClientStream final : public Stream {
 public:
  explicit HttpClientStream(esp_http_client_handle_t client, int64_t contentLength);

  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t) override;
  size_t readBytes(char* buffer, size_t length) override;

  size_t bytesReadCount() const { return bytesRead; }

 private:
  esp_http_client_handle_t client;
  int64_t contentLength;
  size_t bytesRead = 0;
};
