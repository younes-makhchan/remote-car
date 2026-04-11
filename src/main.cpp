/*
  main.cpp — ESP32 Robot Car
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Connection: ESP32 acts as WebSocket CLIENT.
  It connects to FastAPI on your laptop and stays
  connected permanently. Commands arrive as single
  chars over the persistent socket — no HTTP
  handshake overhead per command.

  Protocol (text frames):
    FastAPI → ESP32: "F" forward
                    "B" backward
                    "L" left
                    "R" right
                    "S" stop
                    "0"–"9" speed (0=min 80pwm, 9=max 255pwm)
    ESP32 → FastAPI: "heartbeat" every 10s (keepalive)

  Dependencies (platformio.ini):
    lib_deps =
      ESP Async WebServer          ← still used for WiFi AP fallback (optional)
      links2004/WebSockets @ ^2.4.1  ← WebSocketsClient
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>   // links2004/WebSockets

// ══════════════════════════════════════════════════
//  ★  EDIT THESE  ★
// ══════════════════════════════════════════════════
const char* WIFI_SSID     = "iphone";
const char* WIFI_PASSWORD = "123456789";

// IP or domain of the machine running FastAPI / reverse proxy.
// When you move to EC2, just change this to your EC2 public IP.
const char* FLASK_HOST    = "192.168.137.1";
const uint16_t FLASK_PORT = 5000;
const char*  FLASK_PATH   = "/esp";    // matches @app.websocket("/esp") in app.py
// ══════════════════════════════════════════════════

// Motor pins
const int enA = 5,  enB = 23;
const int IN1 = 22, IN2 = 21, IN3 = 19, IN4 = 18;
const int LED = 2;

// Speed table: digits 0-9 → PWM 80-255
const int SPEED_TABLE[10] = { 80, 97, 114, 131, 148, 165, 182, 199, 226, 255 };
int currentSpeed = 200;   // default
int turnSpeed = 220; // Fixed turning speed for better control
WebSocketsClient wsClient;
bool wsConnected = false;
unsigned long lastHeartbeatMs = 0;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;

// ──────────────────────────────────────────────────
//  MOTOR HELPERS
// ──────────────────────────────────────────────────
void motorStop() {
    digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
    ledcWrite(enA, 0);      ledcWrite(enB, 0);
}

void motorForward() {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
    ledcWrite(enA, currentSpeed); ledcWrite(enB, currentSpeed);
}

void motorBackward() {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
    ledcWrite(enA, currentSpeed); ledcWrite(enB, currentSpeed);
}

void motorRight() {
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
    ledcWrite(enA, turnSpeed); ledcWrite(enB, turnSpeed);
}

void motorLeft() {
    digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
    digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
    ledcWrite(enA, turnSpeed); ledcWrite(enB, turnSpeed);
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
            Serial.printf("[WS] Connected to ws://%s:%d%s\n",
                          FLASK_HOST, FLASK_PORT, FLASK_PATH);
            digitalWrite(LED, HIGH);
            wsClient.sendTXT("heartbeat");
            break;

        case WStype_TEXT: {
            if (length == 0) break;
            char cmd = (char)payload[0];
            Serial.printf("[CMD] %c\n", cmd);

            switch (cmd) {
                case 'F': motorForward();  break;
                case 'B': motorBackward(); break;
                case 'L': motorLeft();     break;
                case 'R': motorRight();    break;
                case 'S': motorStop();     break;
                default:
                    // Speed digit 0-9
                    if (cmd >= '0' && cmd <= '9') {
                        currentSpeed = SPEED_TABLE[cmd - '0'];
                        Serial.printf("[SPEED] %d\n", currentSpeed);
                    }
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
    // Connect to FastAPI. The library auto-reconnects on drop.
    wsClient.begin(FLASK_HOST, FLASK_PORT, FLASK_PATH);
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
