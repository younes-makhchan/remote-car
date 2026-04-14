/*
  main.cpp — car receiver firmware

  The car ESP32 joins the hand controller's SoftAP, connects to its TCP server,
  and applies direct motor mix commands:
    D,left,right\n
    S\n
*/

#include <Arduino.h>
#include <WiFi.h>

// Hand controller SoftAP configuration.
const char* WIFI_SSID = "HandCar-AP";
const char* WIFI_PASSWORD = "handdrive250";
constexpr uint16_t CONTROL_PORT = 4242;

// Motor pins.
const int enA = 5;
const int enB = 23;
const int IN1 = 22;
const int IN2 = 21;
const int IN3 = 19;
const int IN4 = 18;
const int LED = 2;

constexpr int MOTOR_PWM_LIMIT = 250;
constexpr unsigned long WIFI_RETRY_MS = 5000;
constexpr unsigned long TCP_RETRY_MS = 1500;
constexpr unsigned long COMMAND_TIMEOUT_MS = 700;

WiFiClient controlClient;
String inputBuffer;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastTcpAttemptMs = 0;
unsigned long lastCommandMs = 0;

int clampMotorPwm(int value) {
  if (value > MOTOR_PWM_LIMIT) return MOTOR_PWM_LIMIT;
  if (value < -MOTOR_PWM_LIMIT) return -MOTOR_PWM_LIMIT;
  return value;
}

void setLeftMotor(int speed) {
  const int clamped = clampMotorPwm(speed);
  const int pwm = abs(clamped);
  if (clamped > 0) {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
  } else if (clamped < 0) {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);
  } else {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
  }
  ledcWrite(enA, pwm);
}

void setRightMotor(int speed) {
  const int clamped = clampMotorPwm(speed);
  const int pwm = abs(clamped);
  if (clamped > 0) {
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, HIGH);
  } else if (clamped < 0) {
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);
  } else {
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);
  }
  ledcWrite(enB, pwm);
}

void driveMotors(int leftSpeed, int rightSpeed) {
  setLeftMotor(leftSpeed);
  setRightMotor(rightSpeed);
}

void motorStop() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  ledcWrite(enA, 0);
  ledcWrite(enB, 0);
}

bool parseDriveCommand(const String& line, int& left, int& right) {
  if (!line.startsWith("D,")) return false;
  const int firstComma = line.indexOf(',');
  const int secondComma = line.indexOf(',', firstComma + 1);
  if (firstComma < 0 || secondComma < 0) return false;

  const String leftToken = line.substring(firstComma + 1, secondComma);
  const String rightToken = line.substring(secondComma + 1);
  if (!leftToken.length() || !rightToken.length()) return false;

  left = leftToken.toInt();
  right = rightToken.toInt();

  const String normalized = "D," + String(left) + "," + String(right);
  return normalized == line;
}

void handleCommandLine(String line) {
  line.trim();
  if (!line.length()) return;

  if (line == "S") {
    Serial.println("[CTRL] stop");
    motorStop();
    lastCommandMs = millis();
    return;
  }

  int left = 0;
  int right = 0;
  if (!parseDriveCommand(line, left, right)) {
    Serial.printf("[CTRL] invalid payload: %s\n", line.c_str());
    motorStop();
    lastCommandMs = 0;
    return;
  }

  left = clampMotorPwm(left);
  right = clampMotorPwm(right);
  Serial.printf("[CTRL] drive left=%d right=%d\n", left, right);
  driveMotors(left, right);
  lastCommandMs = millis();
}

void processIncomingCommands() {
  while (controlClient.connected() && controlClient.available()) {
    const char ch = static_cast<char>(controlClient.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      handleCommandLine(inputBuffer);
      inputBuffer = "";
      continue;
    }
    if (inputBuffer.length() >= 63) {
      Serial.println("[CTRL] buffer overflow, stopping");
      inputBuffer = "";
      motorStop();
      lastCommandMs = 0;
      continue;
    }
    inputBuffer += ch;
  }
}

void disconnectControlClient() {
  if (controlClient.connected()) {
    controlClient.stop();
  }
  inputBuffer = "";
  lastCommandMs = 0;
  motorStop();
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  const unsigned long now = millis();
  if (now - lastWiFiAttemptMs < WIFI_RETRY_MS) return;
  lastWiFiAttemptMs = now;

  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void ensureTcpConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    disconnectControlClient();
    return;
  }
  if (controlClient.connected()) return;

  const unsigned long now = millis();
  if (now - lastTcpAttemptMs < TCP_RETRY_MS) return;
  lastTcpAttemptMs = now;

  disconnectControlClient();
  Serial.println("[TCP] connecting to hand controller");
  if (controlClient.connect(WiFi.gatewayIP(), CONTROL_PORT)) {
    controlClient.setNoDelay(true);
    inputBuffer = "";
    lastCommandMs = millis();
    Serial.printf("[TCP] connected to %s:%u\n", WiFi.gatewayIP().toString().c_str(), CONTROL_PORT);
  } else {
    Serial.println("[TCP] connect failed");
  }
}

void updateStatusLed() {
  if (controlClient.connected()) {
    digitalWrite(LED, HIGH);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED, (millis() / 250) % 2);
    return;
  }

  digitalWrite(LED, (millis() / 120) % 2);
}

void setup() {
  Serial.begin(115200);

  pinMode(LED, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  motorStop();

  ledcAttach(enA, 5000, 8);
  ledcAttach(enB, 5000, 8);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  ensureWiFiConnected();
}

void loop() {
  ensureWiFiConnected();
  ensureTcpConnected();

  if (WiFi.status() != WL_CONNECTED) {
    disconnectControlClient();
  } else if (controlClient.connected()) {
    processIncomingCommands();
  } else {
    motorStop();
  }

  if (controlClient.connected() && lastCommandMs > 0 && millis() - lastCommandMs > COMMAND_TIMEOUT_MS) {
    Serial.println("[SAFE] command timeout");
    disconnectControlClient();
  }

  updateStatusLed();
  delay(10);
}
