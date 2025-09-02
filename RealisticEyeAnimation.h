#ifndef REALISTIC_EYE_ANIMATION_H
#define REALISTIC_EYE_ANIMATION_H

#include <math.h>
#include <Arduino.h> // 引入Arduino核心库以使用PI常量

// --- 配置 ---
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define MAP_RADIUS 120

// 眨眼状态枚举
typedef enum {
    NOT_BLINKING = 0,
    BLINK_CLOSING = 1,
    BLINK_OPENING = 2
} BlinkState;

// 眼睛状态结构体
typedef struct {
    // 【新增】用于存放每帧平滑移动的当前坐标
    float eyeCurrentX;
    float eyeCurrentY;

    // 用于定义移动起止点的坐标
    float eyeOldX, eyeOldY;
    float eyeNewX, eyeNewY;
    
    // 移动状态
    bool inMotion;
    uint32_t moveStartTimeMs;
    uint32_t moveDurationMs;
    uint32_t lastSaccadeStopMs;
    uint32_t saccadeIntervalMs;
    
    // 眨眼状态
    BlinkState blinkState;
    uint32_t blinkStartTimeMs;
    uint32_t blinkDurationMs;
    float blinkFactor;
    uint32_t timeOfLastBlinkMs;
    uint32_t timeToNextBlinkMs;
    
    // 眼睛开合度
    float eyeOpenness;
} EyeState;

// --- 辅助函数 ---
float random_float(float min, float max) {
    if (min >= max) return min;
    float scale = (float)rand() / (float)RAND_MAX;
    return min + scale * (max - min);
}

// --- 动画核心逻辑 ---

// 初始化眼睛状态
void init_eye_state(EyeState* state) {
    state->eyeOldX = state->eyeNewX = MAP_RADIUS;
    state->eyeOldY = state->eyeNewY = MAP_RADIUS;
    // 【修改】初始化新增的当前坐标
    state->eyeCurrentX = MAP_RADIUS;
    state->eyeCurrentY = MAP_RADIUS;
    
    state->inMotion = false;
    state->moveDurationMs = 0;
    state->moveStartTimeMs = millis();
    state->lastSaccadeStopMs = state->moveStartTimeMs;
    state->saccadeIntervalMs = random(500, 3001);
    
    state->blinkState = NOT_BLINKING;
    state->blinkStartTimeMs = 0;
    state->blinkDurationMs = 0;
    state->blinkFactor = 0.0f;
    state->timeOfLastBlinkMs = millis();
    state->timeToNextBlinkMs = random(2000, 6001);
    
    state->eyeOpenness = 1.0f;
}

// 触发一次眨眼
void trigger_blink(EyeState* state) {
    if (state->blinkState == NOT_BLINKING) {
        state->blinkState = BLINK_CLOSING;
        state->blinkStartTimeMs = millis();
        state->blinkDurationMs = random(50, 101);
    }
}

// 更新自动移动状态
void update_auto_movement(EyeState* state, uint32_t maxGazeMs) {
    uint32_t t = millis();
    uint32_t dt = t - state->moveStartTimeMs;

    if (state->inMotion) {
        // 如果移动时间已到
        if (dt >= state->moveDurationMs) {
            state->inMotion = false;
            // 更新起始点为上一次的目标点
            state->eyeOldX = state->eyeNewX;
            state->eyeOldY = state->eyeNewY;
            // 将当前坐标精确地对齐到目标点
            state->eyeCurrentX = state->eyeNewX;
            state->eyeCurrentY = state->eyeNewY;

            uint32_t limit = (maxGazeMs < 1000) ? maxGazeMs : 1000;
            state->moveDurationMs = random(35, limit + 1);
            
            if (!state->saccadeIntervalMs) {
                state->lastSaccadeStopMs = t;
                state->saccadeIntervalMs = random(state->moveDurationMs, maxGazeMs + 1);
            }
            state->moveStartTimeMs = t;

        } else {
            // 【新增】平滑插值计算
            // 计算移动进度 (0.0 to 1.0)
            float e = (float)dt / (float)state->moveDurationMs;
            // 使用缓动函数，使动作先加速后减速，更自然
            float e2 = e * e;
            e = e2 * (3.0f - 2.0f * e);
            
            // 根据进度计算当前帧的坐标
            state->eyeCurrentX = state->eyeOldX + (state->eyeNewX - state->eyeOldX) * e;
            state->eyeCurrentY = state->eyeOldY + (state->eyeNewY - state->eyeOldY) * e;
        }
    } else { // 如果眼睛当前是静止的
        if (dt > state->moveDurationMs) { // 停留时间结束，开始新的移动
            if ((t - state->lastSaccadeStopMs) > state->saccadeIntervalMs) {
                // 大眼跳 (Saccade) - 【修复坐标计算】
                // 使用固定的移动范围，确保坐标在合理区间内
                float maxRadius = 60.0f; // 固定最大移动半径
                float relativeX = random_float(-maxRadius, maxRadius);
                float maxY = sqrtf(maxRadius * maxRadius - relativeX * relativeX);
                float relativeY = random_float(-maxY, maxY);
                
                state->eyeNewX = MAP_RADIUS + relativeX;
                state->eyeNewY = MAP_RADIUS + relativeY;
                
                // 确保坐标在有效范围内（60-180，适配映射范围56-184）
                state->eyeNewX = constrain(state->eyeNewX, 60, 180);
                state->eyeNewY = constrain(state->eyeNewY, 60, 180);
                
                state->moveDurationMs = random(83, 167);
                state->saccadeIntervalMs = 0;
            } else {
                // 微眼跳 (Microsaccade) - 【修复坐标计算】
                float microRadius = 8.0f; // 固定微移动半径
                float dx = random_float(-microRadius, microRadius);
                float h = sqrtf(microRadius * microRadius - dx * dx);
                float dy = random_float(-h, h);
                
                state->eyeNewX = state->eyeOldX + dx;
                state->eyeNewY = state->eyeOldY + dy;
                
                // 确保坐标在有效范围内
                state->eyeNewX = constrain(state->eyeNewX, 60, 180);
                state->eyeNewY = constrain(state->eyeNewY, 60, 180);
                
                state->moveDurationMs = random(7, 26);
            }
            state->moveStartTimeMs = t;
            state->inMotion = true;
        }
    }
}

void update_auto_blink(EyeState* state) {
    uint32_t t = millis();
    if (t - state->timeOfLastBlinkMs >= state->timeToNextBlinkMs) {
        state->timeOfLastBlinkMs = t;
        trigger_blink(state);
        state->timeToNextBlinkMs = state->blinkDurationMs * 3 + random(2000, 6001);
    }
}

void update_blink_state(EyeState* state) {
    uint32_t t = millis();
    if (state->blinkState != NOT_BLINKING) {
        if (t - state->blinkStartTimeMs >= state->blinkDurationMs) {
            switch (state->blinkState) {
                case BLINK_CLOSING:
                    state->blinkState = BLINK_OPENING;
                    state->blinkDurationMs *= 2;
                    state->blinkStartTimeMs = t;
                    state->blinkFactor = 1.0f;
                    break;
                case BLINK_OPENING:
                    state->blinkState = NOT_BLINKING;
                    state->blinkFactor = 0.0f;
                    break;
            }
        } else {
            float progress = (float)(t - state->blinkStartTimeMs) / (float)state->blinkDurationMs;
            state->blinkFactor = (state->blinkState == BLINK_CLOSING) ? progress : 1.0f - progress;
        }
    }
    state->eyeOpenness = 1.0f - state->blinkFactor;
}

#endif // REALISTIC_EYE_ANIMATION_H