#ifndef UTILS_H
#define UTILS_H

#include <sMQTTBroker.h>
#include <PubSubClient.h>
#include <math.h>      // 用于计算距离
#include <esp_wifi.h>  // 用于识别 wifi_tx_info_t 类型
#include "mapdata.h"

sMQTTBroker broker;
WiFiClient espClient;
PubSubClient client(espClient);


char apSSID[30];
char apPassword[13];
unsigned short mqttPort = 1883;

uint8_t peerMacAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int espnow_channel = 1;

// --- 光照映射函数 (对数曲线版) ---
// lux: 当前传感器读数
// max_in: 你的满量程数值 (这里填 6500)
// offset: 偏移量微调 (-0.5 ~ 0.5)，用于整体平移曲线
// inverse: true = 强光变小(瞳孔模式), false = 强光变大
float map_lux_fast_saturation(float lux, float knee_factor, float offset) {
    // 1. 基础约束
    if (lux < 0) lux = 0;
    
    // 2. Reinhard 色调映射算法
    // 这种算法专门用于将巨大的光照范围压缩到 0~1，且保留低光细节
    // 公式: val = lux / (lux + k)
    // 当 lux = k 时，结果为 0.5
    // 当 lux 远大于 k 时，结果趋近于 1.0
    float normalized = lux / (lux + knee_factor);

    // 3. 目标区间定义 [0.02, 1.0]
    float out_min = 0.02; 
    float out_max = 1.0;
    
    // 4. 映射到目标区间
    // 正向逻辑：光越强，数值越大
    float result = out_min + (normalized * (out_max - out_min));

    // 5. 应用偏移量
    result += offset;

    // 6. 最终约束 (Clamping)
    if (result < out_min) result = out_min;
    if (result > out_max) result = out_max;

    return result;
}

void generateRandomString(char *buffer, int length) {
  const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < length; i++) {
    int randomIndex = random(0, (int)(sizeof(charset) - 1));
    buffer[i] = charset[randomIndex];
  }
  buffer[length] = '\0';
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



void setup_wifi_ap() {
  strcpy(apSSID, "ESP32_controller_");
  char randomPart[7];
  generateRandomString(randomPart, 6);
  strcat(apSSID, randomPart);
  generateRandomString(apPassword, 12);

  WiFi.softAP(apSSID, apPassword, espnow_channel);

  Serial.print("AP 模式启动, SSID: ");
  Serial.print(apSSID);
  Serial.print(" Passwd: ");
  Serial.println(apPassword);
  Serial.print("AP IP 地址: ");
  Serial.println(WiFi.softAPIP());
}

void setup_mqtt_broker() {
  broker.init(mqttPort);
  Serial.println("MQTT Broker 启动成功");
  Serial.print("使用 IP 地址: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("端口: ");
  Serial.println(mqttPort);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("收到MQTT消息 [");
  Serial.print(topic);
  Serial.print("]: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

bool tryConnectMqttClient() {
  client.setServer("127.0.0.1", mqttPort);
  client.setCallback(mqttCallback);

  Serial.print("尝试连接MQTT Broker...");
  if (client.connect("ESPClient")) {
    Serial.println("已连接");
    client.subscribe("controller/callback");
    client.publish("controller/callback", "Hello from Controller");
    return true;
  } else {
    Serial.print("连接失败, rc=");
    Serial.println(client.state());
    return false;
  }
}


// 任务1：维持broker和client状态
void brokerAndClientTask(void *pvParameters) {
  for (;;) {
    broker.update();
    client.loop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// 任务2：周期性重连MQTT
void mqttReconnectTask(void *pvParameters) {
  static unsigned long lastReconnectAttempt = 0;
  for (;;) {
    if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 2000) {
        lastReconnectAttempt = now;
        tryConnectMqttClient();
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}


void setupTasks(){

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();


  setup_wifi_ap();
  setup_espnow();
  delay(1000);
  setup_mqtt_broker();

  xTaskCreatePinnedToCore(
    brokerAndClientTask,
    "BrokerAndClientTask",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  // 创建任务2
  xTaskCreatePinnedToCore(
    mqttReconnectTask,
    "MQTTReconnectTask",
    4096,
    NULL,
    1,
    NULL,
    0
  );
}


// 此函数在手动模式下依然用于为bkl和bkr生成过渡效果数组
void generateArray(float start, float stop, int count, float* buffer) {
    if (count <= 0 || buffer == nullptr) {
        // 无效参数，直接返回
        return;
    }

    if (count == 1) {
        buffer[0] = start;
        return;
    }

    // 计算步长，支持 start > stop
    float step = (stop - start) / (count - 1);

    for (int i = 0; i < count - 1; i++) {
        buffer[i] = start + step * i;
    }

    // 确保最后一个元素精确等于 stop
    buffer[count - 1] = stop;
}


// --- 旧的自动路径生成逻辑已被删除 ---
// 函数 generate_linear_points, generate_next_point 及其相关全局变量
// 已被 RealisticEyeAnimation.h 中的新逻辑所取代。

#endif // UTILS_H