/*
  main.cpp — ESP32 Robot Car
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Joystick logic ported from Arduino RF24 reference sketch.
  X/Y axes arrive as 0..1023 (same as original Arduino analogRead range).

  Motor PWM range: 200..240 (motors stall below 200, cap at 240).
  Zero (stop) is still 0 — motors are either off or in the 200-240 band.

  Protocol (text frames):
    FastAPI → ESP32: "J,x,y"   joystick axes (0..1023 each)
                     "S"        stop
    ESP32 → FastAPI: "heartbeat" every 10s (keepalive)
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/

#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <WebSocketsClient.h>

// ══════════════════════════════════════════════════
//  ★  EDIT THESE  ★
// ══════════════════════════════════════════════════
const char* WIFI_SSID     = "iphone";
const char* WIFI_PASSWORD = "123456789";
const char* FLASK_HOST    = "www.example.com";
const uint16_t FLASK_PORT = 443;
const char*  FLASK_PATH   = "/esp";
// ══════════════════════════════════════════════════

// Motor pins  (unchanged from original)
const int enA = 5,  enB = 23;
const int IN1 = 22, IN2 = 21;   // Motor A
const int IN3 = 19, IN4 = 18;   // Motor B
const int LED = 2;

// ── PWM band ──────────────────────────────────────
//  Any requested speed > 0 is scaled into this band.
//  0 always means stop.
constexpr int PWM_MIN = 200;   // motor stall threshold
constexpr int PWM_MAX = 240;   // hard cap

// ── Joystick dead-zone (matches original Arduino sketch) ──
constexpr int JOY_LOW  = 470;
constexpr int JOY_HIGH = 550;
constexpr int JOY_MAX  = 1023;

// ── Minimum motor speed to suppress buzz below dead-zone ──
constexpr int MOTOR_MIN_SPEED = 70;   // same role as the original sketch

WebSocketsClient wsClient;
bool wsConnected = false;
unsigned long lastHeartbeatMs = 0;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;

// ──────────────────────────────────────────────────
//  scaleToPwmBand
//  Maps a raw speed value (0..255 equivalent) into
//  the 200..240 band, preserving 0 as true stop.
// ──────────────────────────────────────────────────
int scaleToPwmBand(int speed) {
    if (speed <= 0)   return 0;
    if (speed > 255)  speed = 255;
    // linear map: 1..255  →  PWM_MIN..PWM_MAX
    return PWM_MIN + (speed * (PWM_MAX - PWM_MIN)) / 255;
}

// ──────────────────────────────────────────────────
//  LOW-LEVEL MOTOR DRIVERS
//  Accept signed speed: positive = forward, negative = backward.
//  PWM value is always passed through scaleToPwmBand first.
// ──────────────────────────────────────────────────
void setMotorA(int speed) {
    int pwm = scaleToPwmBand(abs(speed));
    if (speed > 0) {
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, HIGH);
    } else if (speed < 0) {
        digitalWrite(IN1, HIGH);
        digitalWrite(IN2, LOW);
    } else {
        digitalWrite(IN1, LOW);
        digitalWrite(IN2, LOW);
        pwm = 0;
    }
    ledcWrite(enA, pwm);
}

void setMotorB(int speed) {
    int pwm = scaleToPwmBand(abs(speed));
    if (speed > 0) {
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, HIGH);
    } else if (speed < 0) {
        digitalWrite(IN3, HIGH);
        digitalWrite(IN4, LOW);
    } else {
        digitalWrite(IN3, LOW);
        digitalWrite(IN4, LOW);
        pwm = 0;
    }
    ledcWrite(enB, pwm);
}

void motorStop() {
    digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    ledcWrite(enA, 0);
    ledcWrite(enB, 0);
}

// ──────────────────────────────────────────────────
//  driveFromJoystick
//  Direct port of the Arduino RF24 receiver sketch logic.
//  Input: x, y in 0..1023 (same as Arduino analogRead).
// ──────────────────────────────────────────────────
void driveFromJoystick(int xAxis, int yAxis) {

    int motorSpeedA = 0;
    int motorSpeedB = 0;

    // ── Y axis → forward / backward ──────────────
    if (yAxis < JOY_LOW) {
        // backward
        motorSpeedA = map(yAxis, JOY_LOW, 0, 0, 255);
        motorSpeedB = map(yAxis, JOY_LOW, 0, 0, 255);
        // direction: backward
        digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
        digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
    }
    else if (yAxis > JOY_HIGH) {
        // forward
        motorSpeedA = map(yAxis, JOY_HIGH, JOY_MAX, 0, 255);
        motorSpeedB = map(yAxis, JOY_HIGH, JOY_MAX, 0, 255);
        // direction: forward
        digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
        digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
    }
    else {
        // Y dead-zone → both speeds start at 0
        motorSpeedA = 0;
        motorSpeedB = 0;
    }

    // ── X axis → steer (blend into A/B speeds) ───
    if (xAxis < JOY_LOW) {
        int xMapped = map(xAxis, JOY_LOW, 0, 0, 255);
        motorSpeedA += xMapped;   // right motor faster
        motorSpeedB -= xMapped;   // left motor slower (or reverse)
        if (motorSpeedA > 255) motorSpeedA = 255;
        if (motorSpeedB < 0)   motorSpeedB = 0;
    }
    else if (xAxis > JOY_HIGH) {
        int xMapped = map(xAxis, JOY_HIGH, JOY_MAX, 0, 255);
        motorSpeedA -= xMapped;   // right motor slower (or reverse)
        motorSpeedB += xMapped;   // left motor faster
        if (motorSpeedA < 0)   motorSpeedA = 0;
        if (motorSpeedB > 255) motorSpeedB = 255;
    }

    // ── Suppress buzz at very low speeds ─────────
    if (motorSpeedA < MOTOR_MIN_SPEED) motorSpeedA = 0;
    if (motorSpeedB < MOTOR_MIN_SPEED) motorSpeedB = 0;

    // ── Handle pure pivot (Y dead-zone + X active) ─
    //  When Y is centered and X is pushed, the direction
    //  pins set above didn't run.  Set them explicitly.
    if (yAxis >= JOY_LOW && yAxis <= JOY_HIGH) {
        if (xAxis < JOY_LOW) {
            // pivot left: A forward, B backward
            digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);  // A forward
            digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);   // B backward
        } else if (xAxis > JOY_HIGH) {
            // pivot right: A backward, B forward
            digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);   // A backward
            digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);  // B forward
        }
        // re-compute pivot speeds (xMapped already applied above)
    }

    // ── Write PWM (scaled into 200-240 band) ─────
    int pwmA = scaleToPwmBand(motorSpeedA);
    int pwmB = scaleToPwmBand(motorSpeedB);

    ledcWrite(enA, pwmA);
    ledcWrite(enB, pwmB);

    Serial.printf("[DRIVE] x=%d y=%d  speedA=%d speedB=%d  pwmA=%d pwmB=%d\n",
                  xAxis, yAxis, motorSpeedA, motorSpeedB, pwmA, pwmB);
}

// ──────────────────────────────────────────────────
//  WEBSOCKET EVENT HANDLER
// ──────────────────────────────────────────────────
bool parseJoystickCommand(const uint8_t* payload, size_t length, int& x, int& y) {
    if (length < 5 || payload[0] != 'J') return false;
    char buffer[32];
    size_t copyLen = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    memcpy(buffer, payload, copyLen);
    buffer[copyLen] = '\0';
    return sscanf(buffer, "J,%d,%d", &x, &y) == 2;
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected — will auto-reconnect");
            wsConnected = false;
            digitalWrite(LED, LOW);
            motorStop();
            break;

        case WStype_CONNECTED:
            wsConnected = true;
            lastHeartbeatMs = millis();
            Serial.printf("[WS] Connected to wss://%s:%d%s\n",
                          FLASK_HOST, FLASK_PORT, FLASK_PATH);
            digitalWrite(LED, HIGH);
            wsClient.sendTXT("heartbeat");
            break;

        case WStype_TEXT: {
            if (length == 0) break;
            char cmd = (char)payload[0];

            if (cmd == 'J') {
                int x = 512, y = 512;
                if (parseJoystickCommand(payload, length, x, y)) {
                    driveFromJoystick(x, y);
                } else {
                    Serial.println("[JOYSTICK] parse error");
                }
                break;
            }

            if (cmd == 'S') {
                motorStop();
                Serial.println("[STOP]");
                break;
            }
            break;
        }

        case WStype_PING:
        case WStype_PONG:
            break;

        default:
            break;
    }
}

// ──────────────────────────────────────────────────
//  SETUP
// ──────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(LED, OUTPUT);
    pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
    motorStop();

    ledcAttach(enA, 5000, 8);
    ledcAttach(enB, 5000, 8);

    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED, !digitalRead(LED));
        delay(300);
        Serial.print(".");
    }
    digitalWrite(LED, HIGH);
    Serial.printf("\nWiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());

    wsClient.beginSSL(FLASK_HOST, FLASK_PORT, FLASK_PATH);
    wsClient.onEvent(onWebSocketEvent);
    wsClient.setReconnectInterval(2000);
}

// ──────────────────────────────────────────────────
//  LOOP
// ──────────────────────────────────────────────────
void loop() {
    wsClient.loop();

    if (wsConnected) {
        unsigned long now = millis();
        if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
            wsClient.sendTXT("heartbeat");
            lastHeartbeatMs = now;
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED, LOW);
        Serial.println("[WiFi] Lost — reconnecting…");
        WiFi.reconnect();
        wsConnected = false;
        delay(5000);
    }
}