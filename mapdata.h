#ifndef MAPDATA_H
#define MAPDATA_H

#include <EEPROM.h>
#define EEPROM_SIZE 32

// TRIGGER BUTTONS
#define LB 20
#define RB 18

// LEFT-JOYSTICK
#define LH 5
#define LV 4
#define LS 21

// RIGHT-JOYSTICK
#define RH 3
#define RV 2
#define RS 15

#define R1 23
#define R2 22

#define I2C_SDA 0
#define I2C_SCL 6

// BATTERY VOLTAGE
//#define ADC 39  //电池ADC

// RGB
//#define DATA_PIN 17

//OTHERS
#define BK 19  //拨动开关

int buttons[7] = {R1, R2, LS, RS, LB, RB, BK};

const int numberOfPotSamples = 5;     // Number of pot samples to take (to smooth the values)
const int delayBetweenSamples = 2;    // Delay in milliseconds between pot samples

int LX_read = 0;
int LY_read = 0;
// int RX_read = 0;
// int RY_read = 0;

byte LX_zero = 127;
byte LY_zero = 127;
// byte RX_zero = 127;
// byte RY_zero = 127;

int LX_to_send = 0;
int LY_to_send = 0;
// int RX_to_send = 0;
// int RY_to_send = 0;

bool LY_inverted = false;
bool LX_inverted = false;
// bool RY_inverted = false;
// bool RX_inverted = false;

int counter = 0;
int invert_counter = 0;

float voltage = 3.00;
int percentage = 0;

void pinmode_pullup() {
  for (int i = 0; i < 7; i++) {
    pinMode(buttons[i], INPUT_PULLUP);
  }
}

/**************************************************/
int map_normal(int val, int lower, int middle, int upper, bool reverse) {
  val = constrain(val, lower, upper);
  if (val < middle)
    val = map(val, lower, middle, 0, 127);
  else
    val = map(val, middle, upper, 127, 255);
  return (reverse ? 255 - val : val);
}

/**************************************************/

void read_joydata() {
  int potValues[numberOfPotSamples];
  for (int i = 0; i < numberOfPotSamples; i++) {
    potValues[i] = analogRead(LH);
    delay(delayBetweenSamples);
  }
  int potValue = 0;
  for (int i = 0; i < numberOfPotSamples; i++) {
    potValue += potValues[i];
  }
  potValue = potValue / numberOfPotSamples;
  LX_read = map(potValue, 0, 4095, 255, 0);

  int potValues2[numberOfPotSamples];
  for (int i = 0; i < numberOfPotSamples; i++) {
    potValues2[i] = analogRead(LV);
    delay(delayBetweenSamples);
  }
  int potValue2 = 0;
  for (int i = 0; i < numberOfPotSamples; i++) {
    potValue2 += potValues2[i];
  }
  potValue2 = potValue2 / numberOfPotSamples;
  LY_read = map(potValue2, 0, 4095, 255, 0);

  // int potValues3[numberOfPotSamples];
  // for (int i = 0; i < numberOfPotSamples; i++) {
  //   potValues3[i] = analogRead(RH);
  //   delay(delayBetweenSamples);
  // }
  // int potValue3 = 0;
  // for (int i = 0; i < numberOfPotSamples; i++) {
  //   potValue3 += potValues3[i];
  // }
  // potValue3 = potValue3 / numberOfPotSamples;
  // RX_read = map(potValue3, 0, 4095, 255, 0);

  // int potValues4[numberOfPotSamples];
  // for (int i = 0; i < numberOfPotSamples; i++) {
  //   potValues4[i] = analogRead(RV);
  //   delay(delayBetweenSamples);
  // }
  // int potValue4 = 0;
  // for (int i = 0; i < numberOfPotSamples; i++) {
  //   potValue4 += potValues4[i];
  // }
  // potValue4 = potValue4 / numberOfPotSamples;
  // RY_read = map(potValue4, 0, 4095, 255, 0);
}

void zero_test() {   //摇杆原点纠偏程序
  Serial.println(" joy_zero_testing... ");
  read_joydata();
  delay(300);

  LX_to_send = map_normal(LX_read, 0, 127, 255, 0);
  LY_to_send = map_normal(LY_read, 0, 127, 255, 0);
  // RX_to_send = map_normal(RX_read, 0, 127, 255, 0);
  // RY_to_send = map_normal(RY_read, 0, 127, 255, 0);

  LX_to_send = constrain(LX_to_send, 0, 255);
  LY_to_send = constrain(LY_to_send, 0, 255);
  // RX_to_send = constrain(RX_to_send, 0, 255);
  // RY_to_send = constrain(RY_to_send, 0, 255);

  LX_zero = LX_to_send;
  LY_zero = LY_to_send;
  // RX_zero = RX_to_send;
  // RY_zero = RY_to_send;

  Serial.println(" Writing in EEPROM... ");
  delay(300);

  if (EEPROM.read(1) != LX_zero) EEPROM.write(1, LX_zero);
  if (EEPROM.read(2) != LY_zero) EEPROM.write(2, LY_zero);
  // if (EEPROM.read(3) != RX_zero) EEPROM.write(3, RX_zero);
  // if (EEPROM.read(4) != RY_zero) EEPROM.write(4, RY_zero);
  EEPROM.commit();

  Serial.println(" Done... ");
  Serial.print(" LX_zero: "); Serial.print(EEPROM.read(1));
  Serial.print(" LY_zero: "); Serial.print(EEPROM.read(2));
  // Serial.print(" RX_zero: "); Serial.print(EEPROM.read(3));
  // Serial.print(" RY_zero: "); Serial.println(EEPROM.read(4));
}

void eeprom_ini() {
  EEPROM.begin(EEPROM_SIZE);

  if (EEPROM.read(0) != 55) {     //判断是否首次使用（新的所有地址貌似是255的值）

    zero_test();

    EEPROM.write(0, 55);
    EEPROM.commit();
  }

  LX_zero = EEPROM.read(1);
  LY_zero = EEPROM.read(2);
  // RX_zero = EEPROM.read(3);
  // RY_zero = EEPROM.read(4);
}

#endif
