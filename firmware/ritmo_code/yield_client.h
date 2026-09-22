#pragma once
// Client HTTP che cede la CPU quando non c'e' niente da leggere.
//
// HTTPClient aspetta le risposte leggendo a vuoto (Stream::timedRead gira su read() fino al timeout,
// 15 s per l'API). Sul core 0 quell'attesa non lasciava girare IDLE0 e il task watchdog (5 s) riavviava
// il dispositivo quando Anthropic rispondeva lento: con il lock di lwIP (TCPIP core locking) il task
// eredita di continuo priorita' alte, quindi non bastava tenerlo a priorita' 0. Qui, a vuoto, 1 ms di pausa.
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

class YieldSecureClient : public NetworkClientSecure {
 public:
  int available() override { int n = NetworkClientSecure::available(); if (n <= 0) vTaskDelay(1); return n; }
  int read() override { int c = NetworkClientSecure::read(); if (c < 0) vTaskDelay(1); return c; }
  int read(uint8_t *buf, size_t size) override { int n = NetworkClientSecure::read(buf, size); if (n <= 0) vTaskDelay(1); return n; }
};

class YieldClient : public NetworkClient {
 public:
  int available() override { int n = NetworkClient::available(); if (n <= 0) vTaskDelay(1); return n; }
  int read() override { int c = NetworkClient::read(); if (c < 0) vTaskDelay(1); return c; }
  int read(uint8_t *buf, size_t size) override { int n = NetworkClient::read(buf, size); if (n <= 0) vTaskDelay(1); return n; }
};
