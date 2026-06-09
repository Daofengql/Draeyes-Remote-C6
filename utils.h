#ifndef UTILS_H
#define UTILS_H

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include "mapdata.h"

uint8_t peerMacAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int espnow_channel = 1;

// --- 光照映射函数 (对数曲线版) ---
// lux: 当前传感器读数
// knee_factor: Reinhard 曲线拐点，越小越快接近满亮
// offset: 偏移量微调，用于整体平移曲线
float map_lux_fast_saturation(float lux, float knee_factor, float offset) {
    if (lux < 0) lux = 0;

    // Reinhard 色调映射: 将较大的光照范围压缩到 0~1。
    float normalized = lux / (lux + knee_factor);

    float out_min = 0.02;
    float out_max = 1.0;
    float result = out_min + (normalized * (out_max - out_min));

    result += offset;

    if (result < out_min) result = out_min;
    if (result > out_max) result = out_max;

    return result;
}

void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW 数据发送失败");
  }
}

void setup_espnow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失败");
    return;
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peerMacAddress, 6);
  peerInfo.channel = espnow_channel;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("添加 ESP-NOW 接收端失败");
  }
}

void setupTasks() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(espnow_channel, WIFI_SECOND_CHAN_NONE);
  setup_espnow();
}

// 此函数在手动模式下用于为 bkl 和 bkr 生成过渡效果数组。
void generateArray(float start, float stop, int count, float* buffer) {
    if (count <= 0 || buffer == nullptr) {
        return;
    }

    if (count == 1) {
        buffer[0] = start;
        return;
    }

    float step = (stop - start) / (count - 1);

    for (int i = 0; i < count - 1; i++) {
        buffer[i] = start + step * i;
    }

    buffer[count - 1] = stop;
}

#endif // UTILS_H
