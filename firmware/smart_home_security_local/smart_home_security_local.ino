/**
 * Smart Home Security System - Local ESP32-CAM Firmware
 *
 * Target board: AI Thinker ESP32-CAM with OV2640 camera
 * Sensors: AM312 PIR, two magnetic door contacts, flame detector, MQ5 module
 *
 * This version intentionally contains no network, messaging, credential, or
 * other cloud code. Alerts and commands use the USB serial interface.
 * Captured JPEG frames are verified in RAM and then released; they are not
 * transmitted or stored.
 *
 * Prototype notice:
 * - This is demonstration firmware, not a certified life-safety system.
 * - Keep every ESP32 input at or below 3.3 V. Use the project's level-shifting
 *   or divider circuit on the MQ5 analog output.
 * - GPIO 2, 12, and 15 are ESP32 boot-strapping pins. The external circuit must
 *   not force an invalid level while the board resets.
 * - GPIO 15 is an ADC2 pin. It works here because Wi-Fi is not enabled. Move
 *   the MQ5 signal to ADC1 hardware if Wi-Fi is added in a future revision.
 */

#include <Arduino.h>
#include "esp_camera.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// AI Thinker ESP32-CAM / OV2640 pin map
// ---------------------------------------------------------------------------

constexpr int CAM_PIN_PWDN = 32;
constexpr int CAM_PIN_RESET = -1;
constexpr int CAM_PIN_XCLK = 0;
constexpr int CAM_PIN_SIOD = 26;
constexpr int CAM_PIN_SIOC = 27;
constexpr int CAM_PIN_D7 = 35;
constexpr int CAM_PIN_D6 = 34;
constexpr int CAM_PIN_D5 = 39;
constexpr int CAM_PIN_D4 = 36;
constexpr int CAM_PIN_D3 = 21;
constexpr int CAM_PIN_D2 = 19;
constexpr int CAM_PIN_D1 = 18;
constexpr int CAM_PIN_D0 = 5;
constexpr int CAM_PIN_VSYNC = 25;
constexpr int CAM_PIN_HREF = 23;
constexpr int CAM_PIN_PCLK = 22;

// ---------------------------------------------------------------------------
// Prototype I/O map retained from the documented build
// ---------------------------------------------------------------------------

constexpr uint8_t FLASH_LED_PIN = 4;
constexpr uint8_t DOOR_1_PIN = 12;
constexpr uint8_t DOOR_2_PIN = 2;
constexpr uint8_t PIR_PIN = 13;
constexpr uint8_t FLAME_PIN = 14;
constexpr uint8_t MQ5_ANALOG_PIN = 15;

constexpr uint8_t DOOR_ACTIVE_LEVEL = LOW;
constexpr uint8_t PIR_ACTIVE_LEVEL = HIGH;
constexpr uint8_t FLAME_ACTIVE_LEVEL = LOW;

constexpr uint32_t DIGITAL_DEBOUNCE_MS = 60;
constexpr uint32_t PIR_AND_MQ5_WARMUP_MS = 40000;
constexpr uint32_t GAS_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t GAS_REPORT_INTERVAL_MS = 5000;

// The original prototype used 400 as its MQ reading. Equality testing was a
// defect, so this version treats it as an adjustable alarm threshold.
// Recalibrate this value for the actual sensor, divider, supply, and clean-air
// baseline. It is not a concentration in ppm.
constexpr uint16_t DEFAULT_GAS_ALARM_THRESHOLD = 400;
constexpr uint16_t GAS_HYSTERESIS = 30;

struct MonitoringState {
  bool doors = true;
  bool motion = true;
  bool flame = true;
  bool gas = true;
};

MonitoringState monitoring;
bool cameraReady = false;
bool flashEnabled = false;

uint16_t gasAlarmThreshold = DEFAULT_GAS_ALARM_THRESHOLD;
uint16_t gasRaw = 0;
uint16_t gasFiltered = 0;
bool gasFilterInitialized = false;
bool gasAlarmActive = false;
bool gasWarmupComplete = false;

uint32_t bootTimeMs = 0;
uint32_t lastGasSampleMs = 0;
uint32_t lastGasReportMs = 0;

char commandBuffer[64];
size_t commandLength = 0;

class DebouncedInput {
 public:
  DebouncedInput(uint8_t pin, uint8_t activeLevel)
      : pin_(pin), activeLevel_(activeLevel) {}

  void begin(uint8_t mode) {
    pinMode(pin_, mode);
    const bool active = readActive();
    rawActive_ = active;
    stableActive_ = active;
    rawChangedAtMs_ = millis();
  }

  // Returns true only after a debounced state transition.
  bool update(uint32_t nowMs) {
    const bool active = readActive();

    if (active != rawActive_) {
      rawActive_ = active;
      rawChangedAtMs_ = nowMs;
    }

    if ((rawActive_ != stableActive_) &&
        (static_cast<uint32_t>(nowMs - rawChangedAtMs_) >=
         DIGITAL_DEBOUNCE_MS)) {
      stableActive_ = rawActive_;
      return true;
    }

    return false;
  }

  bool isActive() const { return stableActive_; }

 private:
  bool readActive() const { return digitalRead(pin_) == activeLevel_; }

  uint8_t pin_;
  uint8_t activeLevel_;
  bool rawActive_ = false;
  bool stableActive_ = false;
  uint32_t rawChangedAtMs_ = 0;
};

DebouncedInput door1Input(DOOR_1_PIN, DOOR_ACTIVE_LEVEL);
DebouncedInput door2Input(DOOR_2_PIN, DOOR_ACTIVE_LEVEL);
DebouncedInput pirInput(PIR_PIN, PIR_ACTIVE_LEVEL);
DebouncedInput flameInput(FLAME_PIN, FLAME_ACTIVE_LEVEL);

void printHelp();
void printStatus();
void processSerialInput();
void handleCommand(char *command);
void updateDigitalSensors(uint32_t nowMs);
void updateGasSensor(uint32_t nowMs);
void reportAlert(const char *message, bool captureImage);
bool initializeCamera();
bool capturePhoto(const char *reason);
void setFlash(bool enabled);

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(20);
  delay(250);

  bootTimeMs = millis();

  door1Input.begin(INPUT_PULLUP);
  door2Input.begin(INPUT_PULLUP);
  pirInput.begin(INPUT);
  flameInput.begin(INPUT);

  pinMode(MQ5_ANALOG_PIN, INPUT);
  analogReadResolution(12);

  pinMode(FLASH_LED_PIN, OUTPUT);
  setFlash(false);

  Serial.println();
  Serial.println(F("Smart Home Security System - local firmware"));
  Serial.println(F("Network and cloud functions are intentionally disabled."));

  cameraReady = initializeCamera();
  Serial.println(cameraReady ? F("[OK] Camera initialized.")
                             : F("[ERROR] Camera unavailable."));

  Serial.println(F("PIR and MQ5 warm-up started (40 s prototype delay)."));
  printHelp();
  printStatus();
}

void loop() {
  const uint32_t nowMs = millis();

  processSerialInput();
  updateDigitalSensors(nowMs);
  updateGasSensor(nowMs);

  delay(2);
}

bool initializeCamera() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_count = 1;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    // The camera driver recommends CIF or lower without PSRAM in JPEG mode.
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("[ERROR] Camera initialization failed: 0x%04X\n",
                  static_cast<unsigned int>(error));
    return false;
  }

  return true;
}

bool capturePhoto(const char *reason) {
  if (!cameraReady) {
    Serial.printf("[PHOTO] Skipped (%s): camera is not ready.\n", reason);
    return false;
  }

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    Serial.printf("[PHOTO] Capture failed (%s).\n", reason);
    return false;
  }

  Serial.printf("[PHOTO] %s: %ux%u JPEG, %u bytes.\n", reason,
                static_cast<unsigned int>(frame->width),
                static_cast<unsigned int>(frame->height),
                static_cast<unsigned int>(frame->len));

  // Integration hook: copy or process frame->buf before returning the buffer.
  esp_camera_fb_return(frame);
  return true;
}

void setFlash(bool enabled) {
  flashEnabled = enabled;
  digitalWrite(FLASH_LED_PIN, flashEnabled ? HIGH : LOW);
}

void reportAlert(const char *message, bool captureImage) {
  Serial.printf("[ALERT %lus] %s\n",
                static_cast<unsigned long>(millis() / 1000UL), message);

  if (captureImage) {
    capturePhoto(message);
  }
}

void updateDigitalSensors(uint32_t nowMs) {
  if (door1Input.update(nowMs) && door1Input.isActive() && monitoring.doors) {
    reportAlert("Door 1 opened", true);
  }

  if (door2Input.update(nowMs) && door2Input.isActive() && monitoring.doors) {
    reportAlert("Door 2 opened", true);
  }

  if (flameInput.update(nowMs) && flameInput.isActive() && monitoring.flame) {
    reportAlert("Flame detector active", false);
  }

  const bool pirChanged = pirInput.update(nowMs);
  const bool warmupElapsed =
      static_cast<uint32_t>(nowMs - bootTimeMs) >= PIR_AND_MQ5_WARMUP_MS;

  if (pirChanged && pirInput.isActive() && monitoring.motion &&
      warmupElapsed) {
    reportAlert("Motion detected", true);
  }
}

void updateGasSensor(uint32_t nowMs) {
  if (static_cast<uint32_t>(nowMs - lastGasSampleMs) <
      GAS_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastGasSampleMs = nowMs;

  gasRaw = static_cast<uint16_t>(analogRead(MQ5_ANALOG_PIN));
  if (!gasFilterInitialized) {
    gasFiltered = gasRaw;
    gasFilterInitialized = true;
  } else {
    gasFiltered = static_cast<uint16_t>(
        (static_cast<uint32_t>(gasFiltered) * 7U + gasRaw) / 8U);
  }

  const bool warmupElapsed =
      static_cast<uint32_t>(nowMs - bootTimeMs) >= PIR_AND_MQ5_WARMUP_MS;

  if (!warmupElapsed) {
    return;
  }

  if (!gasWarmupComplete) {
    gasWarmupComplete = true;
    Serial.println(F("[OK] PIR and MQ5 prototype warm-up delay completed."));
  }

  if (monitoring.gas && !gasAlarmActive &&
      gasFiltered >= gasAlarmThreshold) {
    gasAlarmActive = true;
    reportAlert("MQ5 gas/smoke threshold exceeded", false);
  } else if (gasAlarmActive &&
             gasFiltered <=
                 static_cast<uint16_t>(gasAlarmThreshold - GAS_HYSTERESIS)) {
    gasAlarmActive = false;
    Serial.println(F("[CLEAR] MQ5 reading returned below the reset level."));
  }

  if (static_cast<uint32_t>(nowMs - lastGasReportMs) >=
      GAS_REPORT_INTERVAL_MS) {
    lastGasReportMs = nowMs;
    Serial.printf("[MQ5] raw=%u filtered=%u threshold=%u alarm=%s\n", gasRaw,
                  gasFiltered, gasAlarmThreshold,
                  gasAlarmActive ? "ACTIVE" : "clear");
  }
}

void processSerialInput() {
  while (Serial.available() > 0) {
    const char received = static_cast<char>(Serial.read());

    if (received == '\r') {
      continue;
    }

    if (received == '\n') {
      commandBuffer[commandLength] = '\0';
      if (commandLength > 0) {
        handleCommand(commandBuffer);
      }
      commandLength = 0;
      continue;
    }

    if (isprint(static_cast<unsigned char>(received)) &&
        commandLength < (sizeof(commandBuffer) - 1U)) {
      commandBuffer[commandLength++] =
          static_cast<char>(tolower(static_cast<unsigned char>(received)));
    }
  }
}

bool setMonitoringChannel(const char *channel, bool enabled) {
  if (strcmp(channel, "all") == 0) {
    monitoring.doors = enabled;
    monitoring.motion = enabled;
    monitoring.flame = enabled;
    monitoring.gas = enabled;
    return true;
  }
  if (strcmp(channel, "doors") == 0) {
    monitoring.doors = enabled;
    return true;
  }
  if (strcmp(channel, "motion") == 0) {
    monitoring.motion = enabled;
    return true;
  }
  if (strcmp(channel, "flame") == 0) {
    monitoring.flame = enabled;
    return true;
  }
  if (strcmp(channel, "gas") == 0) {
    monitoring.gas = enabled;
    return true;
  }
  return false;
}

void handleCommand(char *command) {
  char *savePointer = nullptr;
  const char *verb = strtok_r(command, " ", &savePointer);

  if (verb == nullptr) {
    return;
  }

  if (strcmp(verb, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(verb, "status") == 0) {
    printStatus();
    return;
  }

  if (strcmp(verb, "photo") == 0) {
    capturePhoto("manual serial request");
    return;
  }

  if (strcmp(verb, "flash") == 0) {
    const char *state = strtok_r(nullptr, " ", &savePointer);
    if ((state != nullptr) && (strcmp(state, "on") == 0)) {
      setFlash(true);
      Serial.println(F("[OK] Flash enabled."));
    } else if ((state != nullptr) && (strcmp(state, "off") == 0)) {
      setFlash(false);
      Serial.println(F("[OK] Flash disabled."));
    } else {
      Serial.println(F("Usage: flash on|off"));
    }
    return;
  }

  if ((strcmp(verb, "arm") == 0) || (strcmp(verb, "disarm") == 0)) {
    const bool enabled = strcmp(verb, "arm") == 0;
    const char *channel = strtok_r(nullptr, " ", &savePointer);
    if ((channel == nullptr) || !setMonitoringChannel(channel, enabled)) {
      Serial.println(F("Usage: arm|disarm all|doors|motion|flame|gas"));
    } else {
      Serial.printf("[OK] %s monitoring %s.\n", channel,
                    enabled ? "armed" : "disarmed");
    }
    return;
  }

  if (strcmp(verb, "gas-threshold") == 0) {
    const char *valueText = strtok_r(nullptr, " ", &savePointer);
    char *parseEnd = nullptr;
    const unsigned long value =
        (valueText == nullptr) ? 0UL : strtoul(valueText, &parseEnd, 10);

    if ((valueText == nullptr) || (parseEnd == valueText) ||
        (*parseEnd != '\0') || (value < GAS_HYSTERESIS) || (value > 4095UL)) {
      Serial.println(F("Usage: gas-threshold <30..4095>"));
    } else {
      gasAlarmThreshold = static_cast<uint16_t>(value);
      gasAlarmActive = false;
      Serial.printf("[OK] MQ5 threshold set to %u ADC counts.\n",
                    gasAlarmThreshold);
    }
    return;
  }

  Serial.println(F("Unknown command. Enter 'help'."));
}

void printStatus() {
  Serial.println(F("\n--- System status ---"));
  Serial.printf("Camera: %s\n", cameraReady ? "ready" : "unavailable");
  Serial.printf("Flash: %s\n", flashEnabled ? "on" : "off");
  Serial.printf("Monitoring: doors=%s motion=%s flame=%s gas=%s\n",
                monitoring.doors ? "armed" : "off",
                monitoring.motion ? "armed" : "off",
                monitoring.flame ? "armed" : "off",
                monitoring.gas ? "armed" : "off");
  Serial.printf("Inputs: door1=%s door2=%s motion=%s flame=%s\n",
                door1Input.isActive() ? "ACTIVE" : "clear",
                door2Input.isActive() ? "ACTIVE" : "clear",
                pirInput.isActive() ? "ACTIVE" : "clear",
                flameInput.isActive() ? "ACTIVE" : "clear");
  Serial.printf("MQ5: raw=%u filtered=%u threshold=%u warmup=%s\n", gasRaw,
                gasFiltered, gasAlarmThreshold,
                gasWarmupComplete ? "complete" : "active");
  Serial.println(F("---------------------\n"));
}

void printHelp() {
  Serial.println(F("\nSerial commands (115200 baud, newline enabled):"));
  Serial.println(F("  status"));
  Serial.println(F("  photo"));
  Serial.println(F("  flash on|off"));
  Serial.println(F("  arm all|doors|motion|flame|gas"));
  Serial.println(F("  disarm all|doors|motion|flame|gas"));
  Serial.println(F("  gas-threshold <30..4095>"));
  Serial.println(F("  help\n"));
}
