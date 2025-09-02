#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include "utils.h"
#include "mapdata.h"
#include "RealisticEyeAnimation.h" // 包含新的动画逻辑库

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

// 为新的动画系统定义一个全局状态变量
EyeState eyeState; 

unsigned long previousMillis2 = 0;
const int SEND_INTERVAL = 10;
const int JSON_DOC_SIZE = 160;

// R1 连续按下检测相关变量
static unsigned long lastPressTimeR1 = 0;
static int pressCountR1 = 0;
// R2 连续按下检测相关变量
static unsigned long lastPressTimeR2 = 0;
static int pressCountR2 = 0;

// 按键长按检测用
static unsigned long fourButtonPressStart = 0;
static bool fourButtonActionDone = false;
static unsigned long twoButtonPressStart = 0;
static bool twoButtonActionDone = false;

// 用于检测按键的上次状态，实现下降沿检测
bool lastStateR1 = HIGH; 
bool lastStateR2 = HIGH;
bool lastStateLB = HIGH;
bool lastStateRB = HIGH;

// 独立长按检测变量
static unsigned long r1LongPressStart = 0;
static bool r1LongPressDone = false;
static unsigned long r2LongPressStart = 0;
static bool r2LongPressDone = false;

// bkl 处理变量
float bklValues[8];
int bklCount = 0;
int bklIndex = 0;
bool bklActive = false;
float currentBklValue = 1.0f;

// bkr 处理变量
float bkrValues[8];
int bkrCount = 0;
int bkrIndex = 0;
bool bkrActive = false;
float currentBkrValue = 1.0f;

// 【新增】控制模式枚举
enum ControlMode {
  MANUAL_MODE,     // 手动模式
  AUTO_FULL_MODE,  // 完全自动模式 (XY + 眨眼)
  AUTO_BLINK_MODE  // 仅自动眨眼模式 (手动XY + 自动眨眼)
};

ControlMode currentMode = MANUAL_MODE;

// 【新增】摇杆锁定状态
bool joystickLocked = false;

// 切换控制模式 (R1连按三次)
void performSpecialActionR1() {
  // 循环切换三种模式
  switch(currentMode) {
    case MANUAL_MODE:
      currentMode = AUTO_FULL_MODE;
      Serial.println("已进入完全自动控制模式 (XY + 眨眼)");
      break;
    case AUTO_FULL_MODE:
      currentMode = AUTO_BLINK_MODE;
      Serial.println("已进入自动眨眼模式 (手动XY + 自动眨眼)");
      break;
    case AUTO_BLINK_MODE:
      currentMode = MANUAL_MODE;
      Serial.println("已退出自动控制模式");
      break;
  }
}

// 发送 "change" 请求
void performSpecialActionR2() {
  StaticJsonDocument<JSON_DOC_SIZE> doc;
  doc["req"] = "change";
  char jsonBuffer[25];
  size_t jsonLength = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  esp_now_send(peerMacAddress, (uint8_t *)jsonBuffer, jsonLength);
}

// 【修改】R1 长按动作 - 摇杆锁定/解除
void performLongPressR1() {
  // 只在手动模式和自动眨眼模式下有效
  if(currentMode == MANUAL_MODE || currentMode == AUTO_BLINK_MODE) {
    joystickLocked = !joystickLocked;
    if(joystickLocked) {
      Serial.println("R1长按: 摇杆已锁定");
    } else {
      Serial.println("R1长按: 摇杆解除锁定");
    }
  } else {
    Serial.println("R1长按: 当前模式下摇杆锁定功能不可用");
  }
}

// R2 长按动作
void performLongPressR2() {
  Serial.println("R2 长按触发！");
}

// 重置数据结构体
void resetData() {
  data.j1PotX = 127;
  data.j1PotY = 127;
  data.j1Button = 1;
  data.buttonLB = 1;
  data.buttonRB = 1;
  data.tSwitch1 = 1;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinmode_pullup();
  eeprom_ini();
  resetData();

  setupTasks();

  // 初始化随机数生成器
  randomSeed(analogRead(0));
  
  // 初始化新的动画状态
  init_eye_state(&eyeState);
}

void loop() {
    // 读取摇杆数据
    read_joydata();

    // 数据映射和约束
    LX_to_send = map_normal(LX_read, 0, LX_zero, 255, LX_inverted);
    LY_to_send = map_normal(LY_read, 0, LY_zero, 255, LY_inverted);
    LX_to_send = constrain(LX_to_send, 0, 255);
    LY_to_send = constrain(LY_to_send, 0, 255);

    data.j1PotX = LX_to_send;
    data.j1PotY = LY_to_send;
    data.j1Button = digitalRead(LS);
    data.buttonLB = digitalRead(LB);
    data.buttonRB = digitalRead(RB);
    data.tSwitch1 = digitalRead(BK);

    // 读取当前按键状态
    bool currentStateR1 = digitalRead(R1);
    bool currentStateR2 = digitalRead(R2);
    bool currentStateLB = digitalRead(LB);
    bool currentStateRB = digitalRead(RB);
    bool currentStateTSwitch1 = digitalRead(BK);
    
    // 组合键状态判断
    bool allFourPressed = (currentStateR1 == LOW && currentStateR2 == LOW && currentStateLB == LOW && currentStateRB == LOW);
    bool twoButtonPressed = (currentStateR1 == LOW && currentStateR2 == LOW && currentStateLB == HIGH && currentStateRB == HIGH);
    unsigned long now = millis();

    // 四键长按检测 (校准)
    if (allFourPressed) {
        if (fourButtonPressStart == 0) {
            fourButtonPressStart = now;
            fourButtonActionDone = false;
        } else {
            if (!fourButtonActionDone && (now - fourButtonPressStart >= 3000)) {
                zero_test(); // 执行校准
                fourButtonActionDone = true;
            }
        }
    } else {
        fourButtonPressStart = 0;
        fourButtonActionDone = false;
    }

    // 双键长按检测(R1+R2, 配对)
    if (!allFourPressed && twoButtonPressed) {
        if (twoButtonPressStart == 0) {
            twoButtonPressStart = now;
            twoButtonActionDone = false;
        } else {
            if (!twoButtonActionDone && (now - twoButtonPressStart >= 3000)) {
                StaticJsonDocument<JSON_DOC_SIZE> pairDoc;
                pairDoc["req"] = "pairing";
                char pairBuffer[256];
                size_t pairLen = serializeJson(pairDoc, pairBuffer, sizeof(pairBuffer));
                esp_now_send(peerMacAddress, (uint8_t *)pairBuffer, pairLen);
                twoButtonActionDone = true;
            }
        }
    } else if (!twoButtonPressed && !allFourPressed) {
        twoButtonPressStart = 0;
        twoButtonActionDone = false;
    }

    // R1 连续快速按下三次检测
    if (currentStateR1 == LOW && lastStateR1 == HIGH && currentStateR2 == HIGH) {
        if (now - lastPressTimeR1 <= 1000) {
            pressCountR1++;
        } else {
            pressCountR1 = 1;
        }
        lastPressTimeR1 = now;
        if (pressCountR1 >= 3) {
            performSpecialActionR1();
            pressCountR1 = 0;
        }
    }

    // R2 连续快速按下三次检测
    if (currentStateR2 == LOW && lastStateR2 == HIGH && currentStateR1 == HIGH) {
        if (now - lastPressTimeR2 <= 1000) {
            pressCountR2++;
        } else {
            pressCountR2 = 1;
        }
        lastPressTimeR2 = now;
        if (pressCountR2 >= 3) {
            performSpecialActionR2();
            pressCountR2 = 0;
        }
    }

    // 更新按键状态
    lastStateR1 = currentStateR1;
    lastStateR2 = currentStateR2;

    // 检测 LB 的边沿并生成 bkl 数组 (仅在手动模式下有效)
    if (currentMode == MANUAL_MODE && currentStateLB != lastStateLB) {
        if (lastStateLB == HIGH && currentStateLB == LOW) { // 下降沿
            int count = random(6, 8);
            generateArray(1.0f, 0.0f, count, bklValues);
            bklCount = count;
            bklIndex = 0;
            bklActive = true;
        }
        else if (lastStateLB == LOW && currentStateLB == HIGH) { // 上升沿
            int count = random(7, 9);
            generateArray(0.0f, 1.0f, count, bklValues);
            bklCount = count;
            bklIndex = 0;
            bklActive = true;
        }
        lastStateLB = currentStateLB;
    } else if (currentMode != MANUAL_MODE) {
        // 在自动模式下，仍然更新按键状态但不触发动作
        lastStateLB = currentStateLB;
    }

    // 检测 RB 的边沿并生成 bkr 数组 (仅在手动模式下有效)
    if (currentMode == MANUAL_MODE && currentStateRB != lastStateRB) {
        if (lastStateRB == HIGH && currentStateRB == LOW) { // 下降沿
            int count = random(3, 5);
            generateArray(1.0f, 0.0f, count, bkrValues);
            bkrCount = count;
            bkrIndex = 0;
            bkrActive = true;
        }
        else if (lastStateRB == LOW && currentStateRB == HIGH) { // 上升沿
            int count = random(7, 9);
            generateArray(0.0f, 1.0f, count, bkrValues);
            bkrCount = count;
            bkrIndex = 0;
            bkrActive = true;
        }
        lastStateRB = currentStateRB;
    } else if (currentMode != MANUAL_MODE) {
        // 在自动模式下，仍然更新按键状态但不触发动作
        lastStateRB = currentStateRB;
    }

    // 如果 bklActive，逐个读取数组 (仅在手动模式下)
    if (currentMode == MANUAL_MODE && bklActive) {
        if (bklIndex < bklCount) {
            currentBklValue = bklValues[bklIndex];
            bklIndex++;
        } else {
            bklActive = false;
        }
    }

    // 如果 bkrActive，逐个读取数组 (仅在手动模式下)
    if (currentMode == MANUAL_MODE && bkrActive) {
        if (bkrIndex < bkrCount) {
            currentBkrValue = bkrValues[bkrIndex];
            bkrIndex++;
        } else {
            bkrActive = false;
        }
    }

    // 检测 tSwitch1 的状态并处理 bkr (仅在手动模式下)
    if (currentMode == MANUAL_MODE && currentStateTSwitch1 == HIGH) {
        currentBkrValue = currentBklValue;
        bkrActive = false;
    }

    // 独立检测 R1 和 R2 的长按
    if (!(currentStateR1 == LOW && currentStateR2 == LOW)) {
        // 单独检测 R1 长按
        if (currentStateR1 == LOW) {
            if (r1LongPressStart == 0) {
                r1LongPressStart = now;
                r1LongPressDone = false;
            } else {
                if (!r1LongPressDone && (now - r1LongPressStart >= 3000)) {
                    performLongPressR1();
                    r1LongPressDone = true;
                }
            }
        } else {
            r1LongPressStart = 0;
            r1LongPressDone = false;
        }

        // 单独检测 R2 长按
        if (currentStateR2 == LOW) {
            if (r2LongPressStart == 0) {
                r2LongPressStart = now;
                r2LongPressDone = false;
            } else {
                if (!r2LongPressDone && (now - r2LongPressStart >= 3000)) {
                    performLongPressR2();
                    r2LongPressDone = true;
                }
            }
        } else {
            r2LongPressStart = 0;
            r2LongPressDone = false;
        }
    } else {
        // 当 R1 和 R2 同时按下时，重置独立长按检测
        r1LongPressStart = 0;
        r1LongPressDone = false;
        r2LongPressStart = 0;
        r2LongPressDone = false;
    }

    // 周期性发送控制数据
    if (now - previousMillis2 >= SEND_INTERVAL) {
        previousMillis2 = now;
        StaticJsonDocument<JSON_DOC_SIZE> doc;
        doc["req"] = "controller";
        JsonObject dataObj = doc.createNestedObject("data");
        
        // 根据当前模式决定控制方式
        switch(currentMode) {
            case MANUAL_MODE:
                // 完全手动模式
                if(joystickLocked) {
                    // 摇杆锁定时，保持中心位置
                    dataObj["j1PotX"] = 127;
                    dataObj["j1PotY"] = 127;
                } else {
                    // 摇杆未锁定时，使用手动摇杆控制
                    dataObj["j1PotX"] = data.j1PotX;
                    dataObj["j1PotY"] = data.j1PotY;
                }
                dataObj["bkl"] = currentBklValue;
                dataObj["bkr"] = currentBkrValue;
                break;
                
            case AUTO_FULL_MODE:
                // 完全自动模式 (XY + 眨眼)
                update_auto_movement(&eyeState, 3000);
                update_auto_blink(&eyeState);
                update_blink_state(&eyeState);

                {
                    int mappedX = map(eyeState.eyeCurrentX, 60, 180, 0, 255);
                    int mappedY = map(eyeState.eyeCurrentY, 60, 180, 0, 255);
                    mappedX = constrain(mappedX, 0, 255);
                    mappedY = constrain(mappedY, 0, 255);

                    dataObj["j1PotX"] = mappedX;
                    dataObj["j1PotY"] = mappedY;
                    dataObj["bkl"] = eyeState.eyeOpenness;
                    dataObj["bkr"] = eyeState.eyeOpenness;
                }
                break;
                
            case AUTO_BLINK_MODE:
                // 自动眨眼模式 (手动XY + 自动眨眼)
                update_auto_blink(&eyeState);
                update_blink_state(&eyeState);
                
                if(joystickLocked) {
                    // 摇杆锁定时，保持当前位置不变
                    // 可以使用上次的位置或者中心位置
                    dataObj["j1PotX"] = 127;  // 中心位置
                    dataObj["j1PotY"] = 127;  // 中心位置
                } else {
                    // 摇杆未锁定时，使用手动摇杆控制
                    dataObj["j1PotX"] = data.j1PotX;
                    dataObj["j1PotY"] = data.j1PotY;
                }
                dataObj["bkl"] = eyeState.eyeOpenness;  // 使用自动眨眼
                dataObj["bkr"] = eyeState.eyeOpenness;  // 使用自动眨眼
                break;
        }
        
        // 序列化 JSON 并发送
        char jsonBuffer[256];
        size_t jsonLength = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
        esp_now_send(peerMacAddress, (uint8_t *)jsonBuffer, jsonLength);
    }
}