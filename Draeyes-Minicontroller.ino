#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <Wire.h>              
#include "Adafruit_VEML7700.h" 
#include "utils.h"
#include "mapdata.h"
#include "RealisticEyeAnimation.h" 

#define KNEE_FACTOR 35.0
// EEPROM 中存放 Offset 的地址 (避开前 5 个字节)
#define EEPROM_ADDR_OFFSET 10 

// 定义数据包结构体
typedef struct Data_Package {
  byte j1PotX;
  byte j1PotY;
  bool j1Button;
  bool buttonLB;
  bool buttonRB;
  bool tSwitch1;
} Data_Package;
Data_Package data;

Adafruit_VEML7700 veml = Adafruit_VEML7700();

// 全局状态变量
EyeState eyeState; 

// 传感器与光照变量
volatile float currentLux = 0.0;
bool isSensorPresent = false; // 硬件检测标志位

// 调光模式与亮度控制变量
enum DimmingState {
  DIM_OFF,      // 主模式
  DIM_SUB1,     // 调光子模式1 (设置跟随)
  DIM_SUB2      // 调光子模式2 (采样锁定)
};
DimmingState dimmingState = DIM_OFF;

bool lightFollowMode = true;    
float manualLightValue = 1.0f;  
float finalLightToSend = 1.0f; 

// 亮度偏移量 (默认0.0)
float brightnessOffset = 0.0f; 

unsigned long previousMillis2 = 0;
const int SEND_INTERVAL = 10;
const int JSON_DOC_SIZE = 160;

// R1/R2 连续按下检测
static unsigned long lastPressTimeR1 = 0;
static int pressCountR1 = 0;
static unsigned long lastPressTimeR2 = 0;
static int pressCountR2 = 0;

// 长按检测变量
static unsigned long fourButtonPressStart = 0;
static bool fourButtonActionDone = false;
static unsigned long twoButtonPressStart = 0;
static bool twoButtonActionDone = false;

// 按键历史状态
bool lastStateR1 = HIGH; 
bool lastStateR2 = HIGH;
bool lastStateLB = HIGH;
bool lastStateRB = HIGH;

// 独立长按检测
static unsigned long r1LongPressStart = 0;
static bool r1LongPressDone = false;
static unsigned long r2LongPressStart = 0;
static bool r2LongPressDone = false;

// 眨眼特效变量
float bklValues[8];
int bklCount = 0; int bklIndex = 0; bool bklActive = false;
float currentBklValue = 1.0f;

float bkrValues[8];
int bkrCount = 0; int bkrIndex = 0; bool bkrActive = false;
float currentBkrValue = 1.0f;

// 控制模式
enum ControlMode {
  MANUAL_MODE, AUTO_FULL_MODE, AUTO_BLINK_MODE, AUTO_BLINK_LOCKED_MODE
};
ControlMode currentMode = MANUAL_MODE;
bool joystickLocked = false;

// --- 传感器任务 ---
void sensorTask(void *pvParameters) {
  for (;;) {
    // 只有当硬件存在时才读取
    if (isSensorPresent) {
        float rawLux = veml.readLux();
        currentLux = (currentLux * 0.8) + (rawLux * 0.2); 
    }
    vTaskDelay(110 / portTICK_PERIOD_MS); 
  }
}

// --- 辅助功能 ---
void saveOffsetToEEPROM() {
    EEPROM.put(EEPROM_ADDR_OFFSET, brightnessOffset);
    EEPROM.commit();
    Serial.println(" [EEPROM] Offset 已保存");
}

void performSpecialActionR1() {
  switch(currentMode) {
    case MANUAL_MODE: currentMode = AUTO_FULL_MODE; Serial.println("模式: 全自动"); break;
    case AUTO_FULL_MODE: currentMode = AUTO_BLINK_MODE; Serial.println("模式: 自动眨眼"); break;
    case AUTO_BLINK_MODE: currentMode = AUTO_BLINK_LOCKED_MODE; Serial.println("模式: 锁定+眨眼"); break;
    case AUTO_BLINK_LOCKED_MODE: currentMode = MANUAL_MODE; Serial.println("模式: 手动"); break;
  }
}

void performSpecialActionR2() {
  StaticJsonDocument<JSON_DOC_SIZE> doc;
  doc["req"] = "change";
  char jsonBuffer[25];
  size_t jsonLength = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  esp_now_send(peerMacAddress, (uint8_t *)jsonBuffer, jsonLength);
}

// --- R1 长按：切换调光模式 (带硬件检测与保存逻辑) ---
void performLongPressR1() {
  // 1. 如果没有传感器，禁止进入调光模式
  if (!isSensorPresent) {
      Serial.println(">>> 错误: 未检测到光照传感器，无法进入调光模式 <<<");
      return; 
  }

  // 2. 状态切换逻辑
  switch(dimmingState) {
    case DIM_OFF:
      dimmingState = DIM_SUB1;
      Serial.println(">>> 进入调光模式: 子模式1 [设置跟随] <<<");
      Serial.print("    当前 Offset: "); Serial.println(brightnessOffset);
      Serial.println("    操作: LB(+) / RB(-) 微调");
      break;
      
    case DIM_SUB1:
      // 切换前保存 Offset
      saveOffsetToEEPROM();
      dimmingState = DIM_SUB2;
      Serial.println(">>> 切换调光模式: 子模式2 [采样锁定] <<<");
      break;
      
    case DIM_SUB2:
      // 退出前保存 Offset
      saveOffsetToEEPROM();
      dimmingState = DIM_OFF;
      Serial.println("<<< 退出调光模式: 回到主控 <<<");
      break;
  }
}

// --- R2 长按：摇杆锁定 ---
void performLongPressR2() {
  if (dimmingState == DIM_OFF) {
      if(currentMode == MANUAL_MODE || currentMode == AUTO_BLINK_MODE) {
        joystickLocked = !joystickLocked;
        Serial.println(joystickLocked ? "R2长按: 摇杆锁定" : "R2长按: 摇杆解锁");
      }
  }
}

void resetData() {
  data.j1PotX = 127; data.j1PotY = 127;
  data.j1Button = 1; data.buttonLB = 1; data.buttonRB = 1; data.tSwitch1 = 1;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Wire.begin(I2C_SDA, I2C_SCL);
  
  pinmode_pullup();
  
  // 初始化 EEPROM
  eeprom_ini(); 
  resetData();

  // 【读取保存的 Offset】
  EEPROM.get(EEPROM_ADDR_OFFSET, brightnessOffset);
  // 如果读取到的是 NaN (首次使用)，重置为 0
  if (isnan(brightnessOffset)) {
      brightnessOffset = 0.0f;
  }
  Serial.print("加载 Offset: "); Serial.println(brightnessOffset);

  setupTasks();
  randomSeed(analogRead(0));
  init_eye_state(&eyeState);

  // --- 硬件检测逻辑 ---
  if (veml.begin()) {
    isSensorPresent = true;
    Serial.println("Sensor found (VEML7700)");
    
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_100MS); 

    // 只有存在传感器时才创建任务
    xTaskCreatePinnedToCore(sensorTask, "SensorTask", 4096, NULL, 1, NULL, 0);
  } else {
    isSensorPresent = false;
    Serial.println("!!! Sensor NOT found - Light control disabled !!!");
  }
}

void loop() {
    read_joydata();
    unsigned long now = millis();

    // 1. 计算亮度
    float mappedSensorValue = 1.0f; // 默认值
    if (isSensorPresent) {
        // 传入 brightnessOffset 进行计算
        mappedSensorValue = map_lux_fast_saturation(currentLux, KNEE_FACTOR, brightnessOffset);
    }

    // 2. 决定发送值
    if (!isSensorPresent) {
        finalLightToSend = 1.0f; // 无硬件强制为 1
    } else {
        if (lightFollowMode) {
            finalLightToSend = mappedSensorValue;
        } else {
            finalLightToSend = manualLightValue;
        }
    }

    // 读取按键
    bool currentStateR1 = digitalRead(R1);
    bool currentStateR2 = digitalRead(R2);
    bool currentStateLB = digitalRead(LB);
    bool currentStateRB = digitalRead(RB);
    bool currentStateTSwitch1 = digitalRead(BK);

    // ================= 通用层：长按检测 =================
    bool allFourPressed = (currentStateR1 == LOW && currentStateR2 == LOW && currentStateLB == LOW && currentStateRB == LOW);
    bool twoButtonPressed = (currentStateR1 == LOW && currentStateR2 == LOW && currentStateLB == HIGH && currentStateRB == HIGH);

    // R1 长按 (调光模式切换)
    if (!allFourPressed && !twoButtonPressed && currentStateR1 == LOW && currentStateR2 == HIGH) {
        if (r1LongPressStart == 0) { r1LongPressStart = now; r1LongPressDone = false; }
        else if (!r1LongPressDone && (now - r1LongPressStart >= 3000)) {
            performLongPressR1();
            r1LongPressDone = true;
        }
    } else { r1LongPressStart = 0; r1LongPressDone = false; }

    // R2 长按 (摇杆锁定)
    if (!allFourPressed && !twoButtonPressed && currentStateR2 == LOW && currentStateR1 == HIGH) {
        if (r2LongPressStart == 0) { r2LongPressStart = now; r2LongPressDone = false; }
        else if (!r2LongPressDone && (now - r2LongPressStart >= 3000)) {
            performLongPressR2();
            r2LongPressDone = true;
        }
    } else { r2LongPressStart = 0; r2LongPressDone = false; }


    // ================= 模式分流逻辑 =================
    if (dimmingState != DIM_OFF && isSensorPresent) {
        // >>>>>>>>>>>>>>>>> 调光模式 >>>>>>>>>>>>>>>>>
        
        bklActive = false; bkrActive = false; // 禁用眨眼特效

        // 【修改后】LB 检测：增加 Offset (变亮)
        if (lastStateLB == HIGH && currentStateLB == LOW) {
            brightnessOffset += 0.05f;
            if (brightnessOffset > 0.8f) brightnessOffset = 0.8f;
            Serial.print("Offset + : "); Serial.println(brightnessOffset);
        }

        // 【修改后】RB 检测：减少 Offset (变暗)
        if (lastStateRB == HIGH && currentStateRB == LOW) {
            brightnessOffset -= 0.05f;
            if (brightnessOffset < -0.8f) brightnessOffset = -0.8f;
            Serial.print("Offset - : "); Serial.println(brightnessOffset);
        }

        // R2 检测：功能键
        if (lastStateR2 == HIGH && currentStateR2 == LOW) {
            if (dimmingState == DIM_SUB1) {
                lightFollowMode = !lightFollowMode;
                if (!lightFollowMode) {
                    manualLightValue = 1.0f;
                    Serial.println("  [设置] 跟随已关闭 (Light = 1.0)");
                } else {
                    Serial.println("  [设置] 跟随已开启");
                }
            } 
            else if (dimmingState == DIM_SUB2) {
                manualLightValue = mappedSensorValue;
                lightFollowMode = false;
                Serial.print("  [采样] 锁定亮度: "); Serial.println(manualLightValue);
            }
        }

    } else {
        // >>>>>>>>>>>>>>>>> 主控模式 >>>>>>>>>>>>>>>>>
        
        // 1. 组合键
        if (allFourPressed) {
            if (fourButtonPressStart == 0) { fourButtonPressStart = now; fourButtonActionDone = false; }
            else if (!fourButtonActionDone && (now - fourButtonPressStart >= 3000)) { zero_test(); fourButtonActionDone = true; }
        } else { fourButtonPressStart = 0; fourButtonActionDone = false; }

        if (!allFourPressed && twoButtonPressed) {
            if (twoButtonPressStart == 0) { twoButtonPressStart = now; twoButtonActionDone = false; }
            else if (!twoButtonActionDone && (now - twoButtonPressStart >= 3000)) {
                StaticJsonDocument<JSON_DOC_SIZE> pairDoc; pairDoc["req"] = "pairing";
                char pairBuffer[256]; size_t pairLen = serializeJson(pairDoc, pairBuffer, sizeof(pairBuffer));
                esp_now_send(peerMacAddress, (uint8_t *)pairBuffer, pairLen); twoButtonActionDone = true;
            }
        } else if (!twoButtonPressed && !allFourPressed) { twoButtonPressStart = 0; twoButtonActionDone = false; }

        // 2. 三连击
        if (currentStateR1 == LOW && lastStateR1 == HIGH && currentStateR2 == HIGH) {
            if (now - lastPressTimeR1 <= 1000) pressCountR1++; else pressCountR1 = 1;
            lastPressTimeR1 = now;
            if (pressCountR1 >= 3) { performSpecialActionR1(); pressCountR1 = 0; }
        }
        if (currentStateR2 == LOW && lastStateR2 == HIGH && currentStateR1 == HIGH) {
            if (now - lastPressTimeR2 <= 1000) pressCountR2++; else pressCountR2 = 1;
            lastPressTimeR2 = now;
            if (pressCountR2 >= 3) { performSpecialActionR2(); pressCountR2 = 0; }
        }

        // 3. LB/RB 眨眼特效 (仅手动模式)
        if (currentMode == MANUAL_MODE) {
            if (currentStateLB != lastStateLB) {
                if (lastStateLB == HIGH && currentStateLB == LOW) { int count = random(6, 8); generateArray(1.0f, 0.0f, count, bklValues); bklCount = count; bklIndex = 0; bklActive = true; }
                else if (lastStateLB == LOW && currentStateLB == HIGH) { int count = random(7, 9); generateArray(0.0f, 1.0f, count, bklValues); bklCount = count; bklIndex = 0; bklActive = true; }
            }
            if (currentStateRB != lastStateRB) {
                if (lastStateRB == HIGH && currentStateRB == LOW) { int count = random(3, 5); generateArray(1.0f, 0.0f, count, bkrValues); bkrCount = count; bkrIndex = 0; bkrActive = true; }
                else if (lastStateRB == LOW && currentStateRB == HIGH) { int count = random(7, 9); generateArray(0.0f, 1.0f, count, bkrValues); bkrCount = count; bkrIndex = 0; bkrActive = true; }
            }
        }

        // 处理眨眼数组
        if (currentMode == MANUAL_MODE) {
            if (bklActive) { if (bklIndex < bklCount) currentBklValue = bklValues[bklIndex++]; else bklActive = false; }
            if (bkrActive) { if (bkrIndex < bkrCount) currentBkrValue = bkrValues[bkrIndex++]; else bkrActive = false; }
            if (currentStateTSwitch1 == HIGH) { currentBkrValue = currentBklValue; bkrActive = false; }
        }
    }

    // 更新按键状态
    lastStateR1 = currentStateR1; lastStateR2 = currentStateR2;
    lastStateLB = currentStateLB; lastStateRB = currentStateRB;

    // ================= 发送数据 =================
    if (now - previousMillis2 >= SEND_INTERVAL) {
        previousMillis2 = now;
        StaticJsonDocument<JSON_DOC_SIZE> doc;
        doc["req"] = "controller";
        JsonObject dataObj = doc.createNestedObject("data");
        
        dataObj["light"] = finalLightToSend; 

        switch(currentMode) {
            case MANUAL_MODE:
                if (dimmingState != DIM_OFF) { 
                     dataObj["j1PotX"] = 127; dataObj["j1PotY"] = 127;
                     dataObj["bkl"] = 1.0; dataObj["bkr"] = 1.0;
                } else {
                    if(joystickLocked) { dataObj["j1PotX"] = 127; dataObj["j1PotY"] = 127; }
                    else {
                        LX_to_send = map_normal(LX_read, 0, LX_zero, 255, LX_inverted);
                        LY_to_send = map_normal(LY_read, 0, LY_zero, 255, LY_inverted);
                        dataObj["j1PotX"] = constrain(LX_to_send, 0, 255);
                        dataObj["j1PotY"] = constrain(LY_to_send, 0, 255);
                    }
                    dataObj["bkl"] = currentBklValue; dataObj["bkr"] = currentBkrValue;
                }
                break;
                
            case AUTO_FULL_MODE:
                update_auto_movement(&eyeState, 3000); update_auto_blink(&eyeState); update_blink_state(&eyeState);
                {
                    int mx = map(eyeState.eyeCurrentX, 60, 180, 0, 255); int my = map(eyeState.eyeCurrentY, 60, 180, 0, 255);
                    dataObj["j1PotX"] = constrain(mx, 0, 255); dataObj["j1PotY"] = constrain(my, 0, 255);
                    dataObj["bkl"] = eyeState.eyeOpenness; dataObj["bkr"] = eyeState.eyeOpenness;
                }
                break;

            case AUTO_BLINK_MODE:
                update_auto_blink(&eyeState); update_blink_state(&eyeState);
                if(joystickLocked || dimmingState != DIM_OFF) { dataObj["j1PotX"] = 127; dataObj["j1PotY"] = 127; }
                else {
                    LX_to_send = map_normal(LX_read, 0, LX_zero, 255, LX_inverted);
                    LY_to_send = map_normal(LY_read, 0, LY_zero, 255, LY_inverted);
                    dataObj["j1PotX"] = constrain(LX_to_send, 0, 255);
                    dataObj["j1PotY"] = constrain(LY_to_send, 0, 255);
                }
                dataObj["bkl"] = eyeState.eyeOpenness; dataObj["bkr"] = eyeState.eyeOpenness;
                break;

            case AUTO_BLINK_LOCKED_MODE: 
                update_auto_blink(&eyeState); update_blink_state(&eyeState);
                dataObj["j1PotX"] = 127; dataObj["j1PotY"] = 127;
                dataObj["bkl"] = eyeState.eyeOpenness; dataObj["bkr"] = eyeState.eyeOpenness;
                break;
        }
        
        char jsonBuffer[256];
        size_t jsonLength = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        esp_now_send(peerMacAddress, (uint8_t *)jsonBuffer, jsonLength);
    }
}
