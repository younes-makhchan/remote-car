/*
  main.cpp — ESP32 Robot Car
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Connection: ESP32 acts as WebSocket CLIENT.
  It connects to FastAPI on your laptop and stays
  connected permanently.

  Protocol (text frames):
    FastAPI → ESP32: "M,left,right" mixed motor PWM (-255..255 each)
                    "S" stop
    ESP32 → FastAPI: "heartbeat" every 10s (keepalive)

  Dependencies (platformio.ini):
    lib_deps =
      ESP Async WebServer          ← still used for WiFi AP fallback (optional)
      links2004/WebSockets @ ^2.4.1  ← WebSocketsClient
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/

#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <WebSocketsClient.h>   // links2004/WebSockets

// ══════════════════════════════════════════════════
//  ★  EDIT THESE  ★
// ══════════════════════════════════════════════════
const char* WIFI_SSID     = "iphone";
const char* WIFI_PASSWORD = "123456789";

// Public domain exposed by your reverse proxy / TLS terminator.
const char* FLASK_HOST    = "www.example.com";
const uint16_t FLASK_PORT = 443;
const char*  FLASK_PATH   = "/esp";    // matches @app.websocket("/esp") in app.py
// ══════════════════════════════════════════════════

// Motor pins
const int enA = 5,  enB = 23;
const int IN1 = 22, IN2 = 21, IN3 = 19, IN4 = 18;
const int LED = 2;

WebSocketsClient wsClient;
bool wsConnected = false;
unsigned long lastHeartbeatMs = 0;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;
constexpr int MOTOR_PWM_MAX = 255;

// ──────────────────────────────────────────────────
//  MOTOR HELPERS
// ──────────────────────────────────────────────────
int clampMotorPwm(int value) {
    if (value > MOTOR_PWM_MAX) return MOTOR_PWM_MAX;
    if (value < -MOTOR_PWM_MAX) return -MOTOR_PWM_MAX;
    return value;
}

void setLeftMotor(int speed) {
    int clamped = clampMotorPwm(speed);
    int pwm = abs(clamped);
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
    int clamped = clampMotorPwm(speed);
    int pwm = abs(clamped);
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
    digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    ledcWrite(enA, 0);      ledcWrite(enB, 0);
}

bool parseMotorMixCommand(const uint8_t* payload, size_t length, int& left, int& right) {
    if (length < 5 || payload[0] != 'M') return false;
    char buffer[32];
    size_t copyLen = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
    memcpy(buffer, payload, copyLen);
    buffer[copyLen] = '\0';
    return sscanf(buffer, "M,%d,%d", &left, &right) == 2;
}

// ──────────────────────────────────────────────────
//  WEBSOCKET EVENT HANDLER
//  Called by the library on every event.
//  Runs in the main loop — no RTOS needed.
// ──────────────────────────────────────────────────
void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {

        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected — will auto-reconnect");
            wsConnected = false;
            digitalWrite(LED, LOW);
            motorStop();   // safety: stop motors on disconnect
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
            Serial.printf("[CMD] %c\n", cmd);

            if (cmd == 'M') {
                int left = 0;
                int right = 0;
                if (parseMotorMixCommand(payload, length, left, right)) {
                    left = clampMotorPwm(left);
                    right = clampMotorPwm(right);
                    Serial.printf("[MOTOR MIX] left=%d right=%d\n", left, right);
                    driveMotors(left, right);
                } else {
                    Serial.println("[MOTOR MIX] parse error");
                }
                break;
            }

            switch (cmd) {
                case 'S': motorStop();     break;
                default:
                    break;
            }
            break;
        }

        case WStype_PING:
        case WStype_PONG:
            // handled by library automatically
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

    // Motor pins
    pinMode(LED, OUTPUT);
    pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
    motorStop();

    // PWM (ESP32 core v3+)
    ledcAttach(enA, 5000, 8);
    ledcAttach(enB, 5000, 8);

    // ── WiFi ──────────────────────────────────────
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED, !digitalRead(LED));
        delay(300);
        Serial.print(".");
    }
    digitalWrite(LED, HIGH);
    Serial.printf("\nWiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());

    // ── WebSocket client ──────────────────────────
    // Connect to FastAPI through the HTTPS reverse proxy.
    wsClient.beginSSL(FLASK_HOST, FLASK_PORT, FLASK_PATH);
    wsClient.onEvent(onWebSocketEvent);
    wsClient.setReconnectInterval(2000);     // retry every 2s if disconnected
}

// ──────────────────────────────────────────────────
//  LOOP
// ──────────────────────────────────────────────────
void loop() {
    // The library handles reconnects and event dispatch.
    // App-level heartbeats are sent explicitly below.
    wsClient.loop();

    if (wsConnected) {
        unsigned long now = millis();
        if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
            wsClient.sendTXT("heartbeat");
            lastHeartbeatMs = now;
        }
    }

    // WiFi watchdog — reconnect if dropped
    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED, LOW);
        Serial.println("[WiFi] Lost — reconnecting…");
        WiFi.reconnect();
        wsConnected = false;
        delay(5000);
    }
}
