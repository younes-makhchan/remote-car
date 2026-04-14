/*
  hand.cpp — finger-mounted hand controller

  The hand ESP32 boots as a SoftAP, accepts one TCP client from the car, reads
  an MPU6050 over I2C, and translates finger motion into direct motor mix
  commands:
    D,left,right\n
    S\n
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <math.h>

const char* AP_SSID = "HandCar-AP";
const char* AP_PASSWORD = "handdrive250";
constexpr uint16_t CONTROL_PORT = 4242;

constexpr uint8_t MPU6050_ADDR = 0x68;
constexpr uint8_t MPU6050_PWR_MGMT_1 = 0x6B;
constexpr uint8_t MPU6050_ACCEL_CONFIG = 0x1C;
constexpr uint8_t MPU6050_GYRO_CONFIG = 0x1B;
constexpr uint8_t MPU6050_ACCEL_XOUT_H = 0x3B;

constexpr int LED = 2;
constexpr int COMMAND_SPEED = 250;
constexpr int ARC_INNER_SPEED = 110;
constexpr unsigned long CALIBRATION_MS = 2500;
constexpr unsigned long SAMPLE_INTERVAL_MS = 25;
constexpr unsigned long COMMAND_REFRESH_MS = 200;

constexpr float FILTER_ALPHA = 0.22f;
constexpr float PITCH_THRESHOLD_DEG = 10.0f;
constexpr float ROLL_THRESHOLD_DEG = 11.0f;
constexpr float ROTATION_THRESHOLD_DPS = 55.0f;

// Adjust these if the mounted sensor axes are reversed on your finger.
constexpr float PITCH_SIGN = 1.0f;
constexpr float ROLL_SIGN = 1.0f;
constexpr float GYRO_Z_SIGN = 1.0f;

struct ImuSample {
  float pitchDeg;
  float rollDeg;
  float gyroZDps;
};

struct DriveCommand {
  int left;
  int right;
  bool stop;
};

WiFiServer controlServer(CONTROL_PORT);
WiFiClient carClient;
DriveCommand lastSentCommand = {0, 0, true};
unsigned long lastSampleMs = 0;
unsigned long lastSendMs = 0;
float neutralPitchDeg = 0.0f;
float neutralRollDeg = 0.0f;
float gyroZOffset = 0.0f;
float filteredPitchDeg = 0.0f;
float filteredRollDeg = 0.0f;
float filteredGyroZDps = 0.0f;
bool calibrationReady = false;

int clampMotorPwm(int value) {
  if (value > COMMAND_SPEED) return COMMAND_SPEED;
  if (value < -COMMAND_SPEED) return -COMMAND_SPEED;
  return value;
}

bool writeMpuRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readMpuBurst(int16_t& ax, int16_t& ay, int16_t& az, int16_t& gz) {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(MPU6050_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;

  const uint8_t expectedBytes = 14;
  if (Wire.requestFrom(MPU6050_ADDR, expectedBytes) != expectedBytes) return false;

  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  Wire.read();
  Wire.read();
  Wire.read();
  Wire.read();
  gz = (Wire.read() << 8) | Wire.read();
  return true;
}

ImuSample readImuSample() {
  int16_t ax = 0;
  int16_t ay = 0;
  int16_t az = 0;
  int16_t gz = 0;
  if (!readMpuBurst(ax, ay, az, gz)) {
    return {filteredPitchDeg, filteredRollDeg, filteredGyroZDps};
  }

  const float accelX = static_cast<float>(ax) / 16384.0f;
  const float accelY = static_cast<float>(ay) / 16384.0f;
  const float accelZ = static_cast<float>(az) / 16384.0f;
  const float gyroZ = (static_cast<float>(gz) / 131.0f) - gyroZOffset;

  const float pitchDeg = atan2f(accelX, sqrtf((accelY * accelY) + (accelZ * accelZ))) * 180.0f / PI;
  const float rollDeg = atan2f(accelY, accelZ) * 180.0f / PI;

  filteredPitchDeg += (pitchDeg - filteredPitchDeg) * FILTER_ALPHA;
  filteredRollDeg += (rollDeg - filteredRollDeg) * FILTER_ALPHA;
  filteredGyroZDps += (gyroZ - filteredGyroZDps) * FILTER_ALPHA;

  return {
    PITCH_SIGN * (filteredPitchDeg - neutralPitchDeg),
    ROLL_SIGN * (filteredRollDeg - neutralRollDeg),
    GYRO_Z_SIGN * filteredGyroZDps,
  };
}

bool initMpu6050() {
  Wire.begin();
  delay(50);
  if (!writeMpuRegister(MPU6050_PWR_MGMT_1, 0x00)) return false;
  delay(20);
  if (!writeMpuRegister(MPU6050_ACCEL_CONFIG, 0x00)) return false;
  if (!writeMpuRegister(MPU6050_GYRO_CONFIG, 0x00)) return false;
  return true;
}

void calibrateImu() {
  Serial.println("[IMU] keep finger neutral for calibration");
  const unsigned long start = millis();
  float pitchSum = 0.0f;
  float rollSum = 0.0f;
  float gyroSum = 0.0f;
  int samples = 0;

  while (millis() - start < CALIBRATION_MS) {
    int16_t ax = 0;
    int16_t ay = 0;
    int16_t az = 0;
    int16_t gz = 0;
    if (readMpuBurst(ax, ay, az, gz)) {
      const float accelX = static_cast<float>(ax) / 16384.0f;
      const float accelY = static_cast<float>(ay) / 16384.0f;
      const float accelZ = static_cast<float>(az) / 16384.0f;
      const float pitchDeg = atan2f(accelX, sqrtf((accelY * accelY) + (accelZ * accelZ))) * 180.0f / PI;
      const float rollDeg = atan2f(accelY, accelZ) * 180.0f / PI;
      pitchSum += pitchDeg;
      rollSum += rollDeg;
      gyroSum += static_cast<float>(gz) / 131.0f;
      ++samples;
    }
    digitalWrite(LED, (millis() / 120) % 2);
    delay(20);
  }

  if (samples == 0) {
    Serial.println("[IMU] calibration failed");
    return;
  }

  neutralPitchDeg = pitchSum / samples;
  neutralRollDeg = rollSum / samples;
  gyroZOffset = gyroSum / samples;
  filteredPitchDeg = neutralPitchDeg;
  filteredRollDeg = neutralRollDeg;
  filteredGyroZDps = 0.0f;
  calibrationReady = true;

  Serial.printf("[IMU] calibrated pitch=%.2f roll=%.2f gyroZOffset=%.2f\n",
                neutralPitchDeg, neutralRollDeg, gyroZOffset);
}

DriveCommand commandStop() {
  return {0, 0, true};
}

DriveCommand commandRotateLeft() {
  return {-COMMAND_SPEED, COMMAND_SPEED, false};
}

DriveCommand commandRotateRight() {
  return {COMMAND_SPEED, -COMMAND_SPEED, false};
}

DriveCommand commandForward() {
  return {COMMAND_SPEED, COMMAND_SPEED, false};
}

DriveCommand commandBackward() {
  return {-COMMAND_SPEED, -COMMAND_SPEED, false};
}

DriveCommand commandArcLeftForward() {
  return {ARC_INNER_SPEED, COMMAND_SPEED, false};
}

DriveCommand commandArcRightForward() {
  return {COMMAND_SPEED, ARC_INNER_SPEED, false};
}

DriveCommand commandArcLeftBackward() {
  return {-ARC_INNER_SPEED, -COMMAND_SPEED, false};
}

DriveCommand commandArcRightBackward() {
  return {-COMMAND_SPEED, -ARC_INNER_SPEED, false};
}

DriveCommand resolveDriveCommand(const ImuSample& sample) {
  if (fabsf(sample.gyroZDps) >= ROTATION_THRESHOLD_DPS) {
    return sample.gyroZDps > 0 ? commandRotateRight() : commandRotateLeft();
  }

  const bool pitchActive = fabsf(sample.pitchDeg) >= PITCH_THRESHOLD_DEG;
  const bool rollActive = fabsf(sample.rollDeg) >= ROLL_THRESHOLD_DEG;
  if (!pitchActive && !rollActive) return commandStop();

  if (pitchActive) {
    const bool forward = sample.pitchDeg > 0;
    if (!rollActive) return forward ? commandForward() : commandBackward();
    if (forward) return sample.rollDeg > 0 ? commandArcRightForward() : commandArcLeftForward();
    return sample.rollDeg > 0 ? commandArcRightBackward() : commandArcLeftBackward();
  }

  return sample.rollDeg > 0 ? commandArcRightForward() : commandArcLeftForward();
}

String encodeCommand(const DriveCommand& command) {
  if (command.stop) return String("S\n");
  return "D," + String(clampMotorPwm(command.left)) + "," + String(clampMotorPwm(command.right)) + "\n";
}

bool sameCommand(const DriveCommand& a, const DriveCommand& b) {
  return a.stop == b.stop && a.left == b.left && a.right == b.right;
}

void ensureCarClient() {
  if (carClient.connected()) return;

  WiFiClient incoming = controlServer.available();
  if (!incoming) return;

  if (carClient) {
    carClient.stop();
  }
  carClient = incoming;
  carClient.setNoDelay(true);
  lastSendMs = 0;
  lastSentCommand = {0, 0, true};
  Serial.printf("[TCP] car connected: %s\n", carClient.remoteIP().toString().c_str());
}

void sendDriveCommand(const DriveCommand& command) {
  if (!carClient.connected()) return;

  const unsigned long now = millis();
  if (sameCommand(command, lastSentCommand) && now - lastSendMs < COMMAND_REFRESH_MS) {
    return;
  }

  const String payload = encodeCommand(command);
  const size_t sent = carClient.print(payload);
  if (sent != payload.length()) {
    Serial.println("[TCP] send failed");
    carClient.stop();
    return;
  }

  lastSentCommand = command;
  lastSendMs = now;
  Serial.print("[TCP] ");
  Serial.print(payload);
}

void updateLed() {
  if (!calibrationReady) {
    digitalWrite(LED, (millis() / 120) % 2);
    return;
  }
  if (carClient.connected()) {
    digitalWrite(LED, HIGH);
    return;
  }
  digitalWrite(LED, (millis() / 300) % 2);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  if (!initMpu6050()) {
    Serial.println("[IMU] init failed");
    while (true) {
      digitalWrite(LED, !digitalRead(LED));
      delay(100);
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const bool apReady = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!apReady) {
    Serial.println("[WiFi] failed to start SoftAP");
    while (true) {
      digitalWrite(LED, !digitalRead(LED));
      delay(200);
    }
  }

  controlServer.begin();
  controlServer.setNoDelay(true);
  Serial.printf("[WiFi] SoftAP ready ssid=%s ip=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  calibrateImu();
}

void loop() {
  ensureCarClient();
  updateLed();

  const unsigned long now = millis();
  if (!calibrationReady || now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(5);
    return;
  }
  lastSampleMs = now;

  const ImuSample sample = readImuSample();
  const DriveCommand command = resolveDriveCommand(sample);
  sendDriveCommand(command);
}
