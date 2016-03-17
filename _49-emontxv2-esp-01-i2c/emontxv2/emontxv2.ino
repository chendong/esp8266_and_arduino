#include <avr/wdt.h>
#include <Wire.h>
// https://github.com/Makuna/Rtc
#include <RtcDS1307.h>
#include "PinChangeInterrupt.h"
#include "EmonLib.h"

/*
  atmega328  -  esp - ds1307
  3v
  gnd
  scl - d2  - scl
  sda - d0  - sda // led
  d2 -      - sqw
  d6  - tx
  d7  - rst
  d12 -  -        // button to inform flashing of esp
*/

// pins
// IN
#define SQW_PIN         2   // int 0
#define BOOT_MODE_PIN   12  // put esp in booot mode
#define PLS_CNT_PIN     3   // int 1, pulse count
#define DOOR_LOCK_PIN   4   // Onewire ds18b20 is not used  

// OUT
#define LED_PIN         9
#define CHECK_TWI_PIN   6
#define RESET_ESP_PIN   7   // reset esp

typedef struct
{
  uint32_t _salt;
  uint32_t pls_no;
  uint16_t pls_wh;
  uint16_t ct1_wh;
  uint16_t ct2_wh;
  uint16_t ct3_wh;
  uint16_t ct1_vr;
  uint8_t  door;
  uint8_t  pad1;
} data;

data sensor_data;

const int CT2 = 1;
const int CT3 = 1;

EnergyMonitor ct1, ct2, ct3;

RtcDS1307 Rtc;

volatile bool bsqw_pulse;
volatile bool bmode_start;
volatile bool breset_esp01;
volatile bool bboot_done;
volatile bool btwi_idle;
volatile bool btwi_written;
volatile bool bdoor_now;
volatile uint16_t pulseValue = 0;
volatile uint32_t pulseCount = 0;
volatile uint32_t pulseTime;
volatile uint32_t lastTime;

void onpulse_isr() {
  lastTime = pulseTime;
  pulseTime = micros();

  if (( micros() - lastTime ) < 600000 ) {
    return;
  }

  pulseCount++;
  pulseValue = uint16_t((3600000000.0 / (pulseTime - lastTime)) / 0.6);
}

void start_onpulse_isr() {
  detachInterrupt(digitalPinToInterrupt(PLS_CNT_PIN));
  attachInterrupt(digitalPinToInterrupt(PLS_CNT_PIN), onpulse_isr, FALLING);
}

void door_lock_isr() {
  bdoor_now = digitalRead(DOOR_LOCK_PIN);
}

void twi_busy_isr() {
  btwi_idle = digitalRead(CHECK_TWI_PIN);
}

void sqw_isr() {
  bsqw_pulse = true;
  digitalWrite(LED_PIN, HIGH);
}

void boot_mode_isr() {
  bmode_start = !bmode_start;
  breset_esp01 = true;
}

void _reset_esp01(bool upload) {
  bboot_done = false;
  Wire.end();
  digitalWrite(RESET_ESP_PIN, LOW);
  if (upload == true) {
    pinMode(CHECK_TWI_PIN, INPUT_PULLUP);
    pinMode(SDA, OUTPUT);
    pinMode(SCL, INPUT_PULLUP);

    digitalWrite(SDA, LOW);
  } else {
    pinMode(SCL, INPUT_PULLUP);
    pinMode(SDA, INPUT_PULLUP);
  }

  delay(3);
  digitalWrite(RESET_ESP_PIN, HIGH);
  delay(200);

  if (upload == false) {
    bboot_done = true;
    Wire.begin();
  }
}

void start_measure() {
  ct1.calcVI(20, 2000);
  ct2.calcVI(20, 2000);
  ct3.calcVI(20, 2000);

  sensor_data.ct1_wh == ct1.realPower;
  sensor_data.ct2_wh == ct2.realPower;
  sensor_data.ct3_wh == ct3.realPower;
  sensor_data.ct1_vr = ct1.Vrms * 100 ;

  sensor_data._salt++;

  digitalWrite(LED_PIN, LOW);
  btwi_written = false;
}

bool write_nvram() {
  if ( btwi_idle ) {
    detachPinChangeInterrupt(digitalPinToPinChangeInterrupt(CHECK_TWI_PIN));
    pinMode(CHECK_TWI_PIN, OUTPUT);
    digitalWrite(CHECK_TWI_PIN, LOW);
    /* digitalWrite(LED_PIN, HIGH); */
    delayMicroseconds(5);

    // data update
    sensor_data.door   = bdoor_now;
    sensor_data.pls_no = pulseCount;
    sensor_data.pls_wh = pulseValue;
    sensor_data.pad1   = uint8_t(sensor_data.pls_wh + sensor_data.ct1_wh + sensor_data.ct2_wh + sensor_data.ct3_wh + sensor_data.door);

    int start_address = 0;
    const uint8_t* to_write_current = reinterpret_cast<const uint8_t*>(&sensor_data);
    Rtc.SetMemory(start_address, to_write_current, sizeof(sensor_data));

    delayMicroseconds(5);
    /* digitalWrite(LED_PIN, LOW); */
    digitalWrite(CHECK_TWI_PIN, HIGH);
    pinMode(CHECK_TWI_PIN, INPUT_PULLUP);
    attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(CHECK_TWI_PIN), twi_busy_isr, CHANGE);
    return true;
  } else {
    return false;
  }
}

void setup() {
  wdt_enable(WDTO_8S);

  // bool
  bsqw_pulse = bmode_start = breset_esp01 = bboot_done = btwi_written = false;
  btwi_idle = true;

  // pin out
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_ESP_PIN, OUTPUT);

  // pin in
  pinMode(SQW_PIN, INPUT_PULLUP);
  pinMode(BOOT_MODE_PIN, INPUT_PULLUP);
  pinMode(CHECK_TWI_PIN, INPUT_PULLUP);
  pinMode(PLS_CNT_PIN, INPUT_PULLUP);
  pinMode(DOOR_LOCK_PIN, INPUT_PULLUP);

  // pin out
  digitalWrite(RESET_ESP_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);

  // reset esp01
  _reset_esp01(false);
  delay(200);
  digitalWrite(LED_PIN, LOW);

  // sensor_data
  sensor_data._salt  = 0;
  sensor_data.pls_no = 0;
  sensor_data.pls_wh = 0;
  sensor_data.ct1_wh = 3;
  sensor_data.ct2_wh = 4;
  sensor_data.ct3_wh = 5;
  sensor_data.door   = bdoor_now = digitalRead(DOOR_LOCK_PIN);
  sensor_data.pad1   = uint8_t(sensor_data.pls_wh + sensor_data.ct1_wh + sensor_data.ct2_wh + sensor_data.ct3_wh + sensor_data.door);

  // rtc
  Rtc.Begin();
  Rtc.SetIsRunning(true);
  Rtc.SetSquareWavePin(DS1307SquareWaveOut_1Hz);

  ct1.voltageTX(236.2, 1.7);
  ct1.currentTX(1, 111.1);
  ct2.voltageTX(234.26, 1.7);
  ct2.currentTX(2, 111.1);
  ct3.voltageTX(234.26, 1.7);
  ct3.currentTX(3, 111.1);

  // interrupt
  attachInterrupt(digitalPinToInterrupt(SQW_PIN), sqw_isr, FALLING);               //  int 0
  attachInterrupt(digitalPinToInterrupt(PLS_CNT_PIN), start_onpulse_isr, FALLING); //  int 1
  attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(BOOT_MODE_PIN), boot_mode_isr, FALLING);
  attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(CHECK_TWI_PIN), twi_busy_isr, CHANGE);
  attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(DOOR_LOCK_PIN), door_lock_isr, CHANGE);
}

void loop() {
  wdt_reset();
  if (bsqw_pulse ) {
    start_measure();
    if (!breset_esp01 && bboot_done) {
      if (!btwi_written) {
        if (btwi_idle) {
          if (write_nvram()) {
            btwi_written = true;
          }
        }
      }
    }
    bsqw_pulse = false;
  }

  if (breset_esp01) {
    _reset_esp01(bmode_start);
    breset_esp01 = false;
  }
}