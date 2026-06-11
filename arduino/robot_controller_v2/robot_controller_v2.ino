/*
 * robot_controller_v2.ino
 * Mars Rover — Arduino Uno + 4-Channel Relay Firmware (v2)
 *
 * ============================================================
 *  PIN MAP
 * ============================================================
 *  Component           Pin        Notes
 *  -----------------   --------   ----------------------------
 *  Relay 1 (Motor A+)  D4         LEFT-FWD  / RIGHT-BWD
 *  Relay 2 (Motor A-)  D5         LEFT-BWD  / RIGHT-FWD
 *  Relay 3 (Motor B+)  D6         RIGHT-FWD / LEFT-BWD
 *  Relay 4 (Motor B-)  D7         RIGHT-BWD / LEFT-FWD
 *  Servo 1             D9         Pan / Camera
 *  Servo 2             D10        Tilt / Arm
 *  DHT11               D2         Temperature & Humidity
 *  Status LED          D13        Boot blink indicator
 *  HC-05 BT RX (←TX)   A5         Arduino receives from HC-05
 *  HC-05 BT TX (→RX)   A4         Arduino transmits to HC-05
 *  Rain Sensor          A0         Analog rain level
 *  MQ2 Gas Sensor       A1         Analog gas reading
 *  HC-SR04 TRIG         A2         Ultrasonic trigger
 *  HC-SR04 ECHO         A3         Ultrasonic echo
 *
 * ============================================================
 *  RELAY MOTOR TRUTH TABLE
 * ============================================================
 *  Direction   R1(D4)  R2(D5)  R3(D6)  R4(D7)
 *  ---------   ------  ------  ------  ------
 *  STOP        LOW     LOW     LOW     LOW
 *  FORWARD     HIGH    LOW     HIGH    LOW
 *  BACKWARD    LOW     HIGH    LOW     HIGH
 *  LEFT        LOW     HIGH    HIGH    LOW
 *  RIGHT       HIGH    LOW     LOW     HIGH
 *
 * ============================================================
 *  JSON PROTOCOL
 * ============================================================
 *  Receive (from Raspberry Pi / App):
 *    {"dir":"F"}             Move forward
 *    {"dir":"B"}             Move backward
 *    {"dir":"L"}             Turn left
 *    {"dir":"R"}             Turn right
 *    {"dir":"S"}             Stop motors (explicit)
 *    {"s1":90,"s2":45}       Set servo angles (0..180)
 *    {"stop":true}           Emergency stop
 *    {"cfg":"get"}           Dump config as JSON
 *    {"cfg":"map","FL":1,..} Set motor port mapping
 *    {"cfg":"inv","wheel":"FL","invert":1}  Set invert flag
 *    {"cfg":"servo","id":1,"min":0,"max":180,"center":90}
 *    {"cfg":"save"}          Save config to EEPROM
 *    {"cfg":"reset"}         Reset config to defaults
 *    {"cal":"servo","id":1,"angle":90}  Test a servo angle
 *
 *  Send (to Raspberry Pi / App):
 *    {"T":25.5,"H":60.0,"G":312,"D":34,"R":5}  Sensors (every 2s)
 *    {"hb":1}                                    Heartbeat (every 5s)
 *    {"err":"json"}                              Parse error
 *    {"cfg":"dump",...}                           Config dump
 *    {"cfg":"saved"} / {"cfg":"reset_done"}
 *
 * ============================================================
 *  KEY FIXES IN V2 (vs V1)
 * ============================================================
 *  1. RX-PRIORITY ARCHITECTURE: Always drain btSerial RX buffer
 *     BEFORE any transmission. Defer TX if incoming data pending.
 *     This fixes the "send 5 commands for 1 to work" problem.
 *
 *  2. NO AUTO-FAILSAFE: Removed the 800ms motor timeout.
 *     Motors run until explicit {"dir":"S"} or {"stop":true}.
 *     Matches the press-and-hold UI on the Pi side.
 *
 *  3. GUARDED TX: Before every btSerial.print(), check for and
 *     drain any pending RX bytes. After TX, call btSerial.listen()
 *     to immediately switch back to receive mode.
 *
 *  4. LONGER TX INTERVALS: Sensors every 2000ms (was 1000ms),
 *     heartbeat every 5000ms (was 2000ms). Less TX = less RX loss.
 *
 *  5. LARGER CMD BUFFER: 256 bytes (was 128) to handle any JSON.
 *
 *  6. RELAY MOTOR DRIVER: Replaced AFMotor shield with direct
 *     4-channel relay control on D4-D7.
 */

// ===== LIBRARIES =====
#include <Servo.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>   // v6.x — StaticJsonDocument
#include <DHT.h>
#include <EEPROM.h>

// ===== PIN MAP =====
#define RELAY1_PIN  4   // Motor A forward
#define RELAY2_PIN  5   // Motor A backward
#define RELAY3_PIN  6   // Motor B forward
#define RELAY4_PIN  7   // Motor B backward

#define SERVO1_PIN  9   // Pan / Camera servo
#define SERVO2_PIN  10  // Tilt / Arm servo

#define DHT_PIN     2
#define DHT_TYPE    DHT11

#define RAIN_PIN    A0
#define MQ2_PIN     A1
#define TRIG_PIN    A2
#define ECHO_PIN    A3
// A4 = btSerial TX  (Arduino → HC-05 RX via voltage divider)
// A5 = btSerial RX  (HC-05 TX → Arduino)
#define LED_PIN     13

// ===== TIMING CONSTANTS =====
// Increased intervals to reduce TX frequency and minimize RX data loss
#define SENSOR_INTERVAL_MS  2000UL   // Was 1000ms in v1
#define HEARTBEAT_INTERVAL_MS 5000UL // Was 2000ms in v1

// ===== COMMAND BUFFER =====
// Larger buffer to handle complex JSON payloads without overflow
#define CMD_BUFFER_SIZE 256

// ===== EEPROM CONFIG =====
#define CFG_MAGIC   0xA5
#define CFG_VERSION 2       // Bumped for v2 (relay-based)
#define CFG_ADDR    0

struct RobotConfig {
    uint8_t magic;
    uint8_t version;
    // Motor port mapping: which relay pair drives each logical wheel
    // For relay-based control these map to relay pairs, kept for
    // compatibility with config protocol
    uint8_t port_FL;   // Front-Left
    uint8_t port_FR;   // Front-Right
    uint8_t port_RL;   // Rear-Left
    uint8_t port_RR;   // Rear-Right
    // Direction invert per wheel (0=normal, 1=invert)
    uint8_t inv_FL;
    uint8_t inv_FR;
    uint8_t inv_RL;
    uint8_t inv_RR;
    // Servo 1 limits
    uint8_t s1_min;
    uint8_t s1_max;
    uint8_t s1_center;
    uint8_t s1_invert;
    // Servo 2 limits
    uint8_t s2_min;
    uint8_t s2_max;
    uint8_t s2_center;
    uint8_t s2_invert;
    // General
    uint8_t default_speed;  // Kept for protocol compat (relay = on/off)
    uint8_t checksum;       // XOR of all preceding bytes
};

RobotConfig cfg;

// ===== OBJECTS =====
// SoftwareSerial: A5 = RX (receive from HC-05), A4 = TX (send to HC-05)
SoftwareSerial btSerial(A5, A4);

DHT dht(DHT_PIN, DHT_TYPE);

Servo servo1;
Servo servo2;

// ===== STATE =====
unsigned long last_sensor_ms = 0;
unsigned long last_hb_ms     = 0;
bool motors_active           = false;  // Track if motors are currently running

// Command buffer — accumulates incoming chars until newline
char cmd_buffer[CMD_BUFFER_SIZE];
uint8_t cmd_index = 0;

// TX deferral flag — set when sensor/heartbeat TX is due but
// RX data is pending; we'll retry next loop iteration
bool tx_sensor_pending = false;
bool tx_hb_pending     = false;

// ===== EEPROM FUNCTIONS =====

// Compute XOR checksum over all config bytes except the checksum field
uint8_t calcChecksum(const RobotConfig &c) {
    const uint8_t *p = (const uint8_t *)&c;
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(RobotConfig) - 1; i++) {
        x ^= p[i];
    }
    return x;
}

// Load factory default configuration
void loadDefaults() {
    cfg.magic   = CFG_MAGIC;
    cfg.version = CFG_VERSION;
    cfg.port_FL = 1; cfg.port_FR = 2;
    cfg.port_RL = 3; cfg.port_RR = 4;
    cfg.inv_FL  = 0; cfg.inv_FR  = 0;
    cfg.inv_RL  = 0; cfg.inv_RR  = 0;
    cfg.s1_min  = 0; cfg.s1_max  = 180; cfg.s1_center = 90; cfg.s1_invert = 0;
    cfg.s2_min  = 0; cfg.s2_max  = 180; cfg.s2_center = 90; cfg.s2_invert = 0;
    cfg.default_speed = 200;
    cfg.checksum = 0;  // Set properly by saveConfig()
}

// Save current config to EEPROM with updated checksum
void saveConfig() {
    cfg.magic    = CFG_MAGIC;
    cfg.version  = CFG_VERSION;
    cfg.checksum = calcChecksum(cfg);
    EEPROM.put(CFG_ADDR, cfg);  // Only writes changed bytes (wear leveling)
}

// Load config from EEPROM; returns true if valid, false if defaults were written
bool loadConfig() {
    EEPROM.get(CFG_ADDR, cfg);
    if (cfg.magic   != CFG_MAGIC   ||
        cfg.version != CFG_VERSION ||
        cfg.checksum != calcChecksum(cfg)) {
        loadDefaults();
        saveConfig();
        return false;
    }
    return true;
}

// ===== RELAY MOTOR FUNCTIONS =====
// Relay-based motor control: each relay is simply on (HIGH) or off (LOW).
// No PWM speed control — the relay module switches full power.

// Stop all motors — all relays LOW
void stopAll() {
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);
    digitalWrite(RELAY3_PIN, LOW);
    digitalWrite(RELAY4_PIN, LOW);
    motors_active = false;
}

// Set motor direction using the relay truth table
// dir: 'F'=forward, 'B'=backward, 'L'=left, 'R'=right, 'S'=stop
void setMotorDirection(char dir) {
    switch (dir) {
        case 'F':  // FORWARD: R1=HIGH R2=LOW R3=HIGH R4=LOW
            digitalWrite(RELAY1_PIN, HIGH);
            digitalWrite(RELAY2_PIN, LOW);
            digitalWrite(RELAY3_PIN, HIGH);
            digitalWrite(RELAY4_PIN, LOW);
            motors_active = true;
            break;

        case 'B':  // BACKWARD: R1=LOW R2=HIGH R3=LOW R4=HIGH
            digitalWrite(RELAY1_PIN, LOW);
            digitalWrite(RELAY2_PIN, HIGH);
            digitalWrite(RELAY3_PIN, LOW);
            digitalWrite(RELAY4_PIN, HIGH);
            motors_active = true;
            break;

        case 'L':  // LEFT: R1=LOW R2=HIGH R3=HIGH R4=LOW
            digitalWrite(RELAY1_PIN, LOW);
            digitalWrite(RELAY2_PIN, HIGH);
            digitalWrite(RELAY3_PIN, HIGH);
            digitalWrite(RELAY4_PIN, LOW);
            motors_active = true;
            break;

        case 'R':  // RIGHT: R1=HIGH R2=LOW R3=LOW R4=HIGH
            digitalWrite(RELAY1_PIN, HIGH);
            digitalWrite(RELAY2_PIN, LOW);
            digitalWrite(RELAY3_PIN, LOW);
            digitalWrite(RELAY4_PIN, HIGH);
            motors_active = true;
            break;

        case 'S':  // STOP: all LOW
        default:
            stopAll();
            break;
    }
}

// ===== SERVO FUNCTIONS =====

// Apply servo limits and optional inversion
int applyServoLimits(int angle, uint8_t mn, uint8_t mx, uint8_t inv) {
    if (inv) angle = 180 - angle;
    return constrain(angle, mn, mx);
}

void writeServo1(int angle) {
    servo1.write(applyServoLimits(angle, cfg.s1_min, cfg.s1_max, cfg.s1_invert));
}

void writeServo2(int angle) {
    servo2.write(applyServoLimits(angle, cfg.s2_min, cfg.s2_max, cfg.s2_invert));
}

// ===== ULTRASONIC SENSOR =====

long readUltrasonicCm() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    unsigned long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);  // 30ms timeout ~5m
    if (dur == 0) return 0;
    return (long)(dur * 0.034 / 2.0);
}

// ===== GUARDED BLUETOOTH TX =====
// These functions implement the RX-priority architecture.
// Before ANY transmission over btSerial, we:
//   1. Drain all pending RX bytes into the command buffer
//   2. Transmit the data
//   3. Immediately call btSerial.listen() to switch back to RX mode
// This minimizes the window where incoming data could be lost.

// Drain all currently available bytes from btSerial into cmd_buffer.
// If a complete command (newline) is found, process it immediately.
// Returns: number of bytes drained
uint8_t drainRxBuffer() {
    uint8_t count = 0;
    while (btSerial.available()) {
        char c = (char)btSerial.read();
        count++;
        if (c == '\n' || c == '\r') {
            if (cmd_index > 0) {
                cmd_buffer[cmd_index] = '\0';
                processJson(cmd_buffer);
                cmd_index = 0;
            }
        } else {
            if (cmd_index < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_index++] = c;
            }
            // Overflow protection: discard char if buffer full
        }
    }
    return count;
}

// Safe transmit: drain RX first, send data, then switch back to listen mode.
// If RX data was pending, it gets processed before we transmit.
void guardedSendSensors() {
    // Step 1: Drain any pending RX data before we block the line
    drainRxBuffer();

    // Step 2: Check AGAIN — if more data arrived during drain, defer TX
    if (btSerial.available()) {
        tx_sensor_pending = true;  // Will retry next loop
        return;
    }

    // Step 3: Build and transmit sensor JSON
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int   g = analogRead(MQ2_PIN);
    int   r = constrain(map(analogRead(RAIN_PIN), 0, 1023, 100, 0), 0, 100);
    long  d = readUltrasonicCm();

    StaticJsonDocument<128> doc;
    if (!isnan(t)) doc[F("T")] = (float)((int)(t * 10 + 0.5)) / 10.0;
    if (!isnan(h)) doc[F("H")] = (float)((int)(h * 10 + 0.5)) / 10.0;
    doc[F("G")] = g;
    doc[F("R")] = r;
    if (d > 0) doc[F("D")] = d;

    serializeJson(doc, btSerial);
    btSerial.println();

    // Step 4: Immediately switch back to receive mode
    btSerial.listen();

    tx_sensor_pending = false;
}

// Safe heartbeat transmission with RX guard
void guardedSendHeartbeat() {
    // Step 1: Drain pending RX
    drainRxBuffer();

    // Step 2: Defer if more data incoming
    if (btSerial.available()) {
        tx_hb_pending = true;
        return;
    }

    // Step 3: Transmit heartbeat
    btSerial.println(F("{\"hb\":1}"));

    // Step 4: Switch back to receive
    btSerial.listen();

    tx_hb_pending = false;
}

// Generic guarded print — used for config responses, error messages, etc.
// Drains RX, transmits, then re-listens.
void guardedPrintln(const __FlashStringHelper *msg) {
    drainRxBuffer();
    btSerial.println(msg);
    btSerial.listen();
}

// Guarded JSON serialization output
void guardedSerializeJson(StaticJsonDocument<256> &doc) {
    drainRxBuffer();
    serializeJson(doc, btSerial);
    btSerial.println();
    btSerial.listen();
}

// ===== CONFIG DUMP =====

void dumpConfig() {
    StaticJsonDocument<256> doc;
    doc[F("cfg")]          = F("dump");
    doc[F("port_FL")]      = cfg.port_FL;
    doc[F("port_FR")]      = cfg.port_FR;
    doc[F("port_RL")]      = cfg.port_RL;
    doc[F("port_RR")]      = cfg.port_RR;
    doc[F("inv_FL")]       = cfg.inv_FL;
    doc[F("inv_FR")]       = cfg.inv_FR;
    doc[F("inv_RL")]       = cfg.inv_RL;
    doc[F("inv_RR")]       = cfg.inv_RR;
    doc[F("s1_min")]       = cfg.s1_min;
    doc[F("s1_max")]       = cfg.s1_max;
    doc[F("s1_center")]    = cfg.s1_center;
    doc[F("s1_invert")]    = cfg.s1_invert;
    doc[F("s2_min")]       = cfg.s2_min;
    doc[F("s2_max")]       = cfg.s2_max;
    doc[F("s2_center")]    = cfg.s2_center;
    doc[F("s2_invert")]    = cfg.s2_invert;
    doc[F("default_speed")] = cfg.default_speed;
    doc[F("ver")]          = cfg.version;
    guardedSerializeJson(doc);
}

// ===== HANDLE CFG COMMAND =====

void handleCfg(StaticJsonDocument<256> &doc) {
    const char *cmd = doc[F("cfg")];
    if (!cmd) return;

    if (!strcmp(cmd, "get")) {
        dumpConfig();
    }
    else if (!strcmp(cmd, "map")) {
        // Set logical-to-physical wheel port mapping
        if (doc.containsKey("FL")) cfg.port_FL = (uint8_t)(int)doc["FL"];
        if (doc.containsKey("FR")) cfg.port_FR = (uint8_t)(int)doc["FR"];
        if (doc.containsKey("RL")) cfg.port_RL = (uint8_t)(int)doc["RL"];
        if (doc.containsKey("RR")) cfg.port_RR = (uint8_t)(int)doc["RR"];
    }
    else if (!strcmp(cmd, "inv")) {
        const char *w = doc["wheel"];
        if (!w) return;
        uint8_t v = doc.containsKey("invert") ? (uint8_t)(int)doc["invert"] : 0;
        if      (!strcmp(w, "FL")) cfg.inv_FL = v;
        else if (!strcmp(w, "FR")) cfg.inv_FR = v;
        else if (!strcmp(w, "RL")) cfg.inv_RL = v;
        else if (!strcmp(w, "RR")) cfg.inv_RR = v;
    }
    else if (!strcmp(cmd, "servo")) {
        uint8_t id = doc.containsKey("id") ? (uint8_t)(int)doc["id"] : 1;
        if (id == 1) {
            if (doc.containsKey("min"))    cfg.s1_min    = (uint8_t)(int)doc["min"];
            if (doc.containsKey("max"))    cfg.s1_max    = (uint8_t)(int)doc["max"];
            if (doc.containsKey("center")) cfg.s1_center = (uint8_t)(int)doc["center"];
            if (doc.containsKey("invert")) cfg.s1_invert = (uint8_t)(int)doc["invert"];
        } else {
            if (doc.containsKey("min"))    cfg.s2_min    = (uint8_t)(int)doc["min"];
            if (doc.containsKey("max"))    cfg.s2_max    = (uint8_t)(int)doc["max"];
            if (doc.containsKey("center")) cfg.s2_center = (uint8_t)(int)doc["center"];
            if (doc.containsKey("invert")) cfg.s2_invert = (uint8_t)(int)doc["invert"];
        }
    }
    else if (!strcmp(cmd, "save")) {
        saveConfig();
        guardedPrintln(F("{\"cfg\":\"saved\"}"));
    }
    else if (!strcmp(cmd, "reset")) {
        loadDefaults();
        saveConfig();
        guardedPrintln(F("{\"cfg\":\"reset_done\"}"));
    }
}

// ===== HANDLE CAL COMMAND =====

void handleCal(StaticJsonDocument<256> &doc) {
    const char *what = doc["cal"];
    if (!what) return;

    if (!strcmp(what, "servo")) {
        // Test a specific servo angle
        uint8_t id = doc.containsKey("id") ? (uint8_t)(int)doc["id"] : 1;
        int angle  = doc.containsKey("angle") ? (int)doc["angle"] : 90;
        if (id == 1) writeServo1(angle);
        else         writeServo2(angle);
    }
    // Note: "port" and "dir" calibration commands from v1 used AFMotor
    // and are not applicable to relay-based control. Relay motors are
    // on/off only — no individual port speed testing needed.
}

// ===== MAIN JSON PROCESSOR =====

void processJson(const char *s) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, s);
    if (err) {
        guardedPrintln(F("{\"err\":\"json\"}"));
        Serial.print(F("JSON err: "));
        Serial.println(err.c_str());
        return;
    }

    // --- Config commands ---
    if (doc.containsKey("cfg")) {
        handleCfg(doc);
        return;
    }

    // --- Calibration commands ---
    if (doc.containsKey("cal")) {
        handleCal(doc);
        return;
    }

    // --- Direction command (relay-based) ---
    // FIX #2: No failsafe timer. Motors run until explicit "S" or "stop".
    if (doc.containsKey("dir")) {
        const char *d = doc["dir"];
        if (d && d[0] != '\0') {
            setMotorDirection(d[0]);  // F, B, L, R, or S
        }
        return;
    }

    // --- Emergency stop ---
    if (doc.containsKey("stop") && (bool)doc["stop"] == true) {
        stopAll();
        return;
    }

    // --- Servo control (can be combined or standalone) ---
    if (doc.containsKey("s1")) writeServo1((int)doc["s1"]);
    if (doc.containsKey("s2")) writeServo2((int)doc["s2"]);
}

// ===== SETUP =====

void setup() {
    Serial.begin(9600);     // USB debug console
    btSerial.begin(9600);   // HC-05 Bluetooth at default baud

    dht.begin();

    // Configure relay pins as outputs (start LOW = motors off)
    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    pinMode(RELAY3_PIN, OUTPUT);
    pinMode(RELAY4_PIN, OUTPUT);
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);
    digitalWrite(RELAY3_PIN, LOW);
    digitalWrite(RELAY4_PIN, LOW);

    // Ultrasonic sensor pins
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // Status LED
    pinMode(LED_PIN, OUTPUT);

    // Attach servos
    servo1.attach(SERVO1_PIN);
    servo2.attach(SERVO2_PIN);

    // Load configuration from EEPROM (or write defaults if invalid)
    bool loaded = loadConfig();

    // Ensure motors are stopped on boot
    stopAll();

    // Move servos to configured center positions
    writeServo1(cfg.s1_center);
    writeServo2(cfg.s2_center);

    // Blink LED: 2 blinks = config loaded, 3 blinks = defaults written
    int blinks = loaded ? 2 : 3;
    for (int i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(100);
        digitalWrite(LED_PIN, LOW);  delay(100);
    }

    // Initialize timing baselines
    last_sensor_ms = millis();
    last_hb_ms     = millis();

    // Ensure btSerial is in receive mode from the start
    btSerial.listen();

    Serial.println(F("Mars Rover v2 Boot OK"));
}

// ===== MAIN LOOP =====
// Architecture: RX-PRIORITY
//   1. ALWAYS drain the RX buffer first (commands processed inline)
//   2. Only transmit sensor/heartbeat data when no RX data is pending
//   3. All TX operations are guarded (drain→TX→listen)
//   4. No motor failsafe timer — explicit stop commands only

void loop() {
    // -------------------------------------------------------
    // STEP 1: DRAIN ALL PENDING RX DATA (highest priority)
    // -------------------------------------------------------
    // Read every available byte from btSerial. If a complete
    // JSON command (terminated by newline) is found, it is
    // processed immediately inside drainRxBuffer().
    drainRxBuffer();

    unsigned long now = millis();

    // -------------------------------------------------------
    // STEP 2: SENSOR TELEMETRY (every 2000ms)
    // -------------------------------------------------------
    // Only transmit if interval elapsed AND no RX data pending.
    // If RX data is pending, set tx_sensor_pending flag to retry.
    if (tx_sensor_pending || (now - last_sensor_ms >= SENSOR_INTERVAL_MS)) {
        if (btSerial.available()) {
            // RX data pending — defer transmission, prioritize receiving
            tx_sensor_pending = true;
        } else {
            guardedSendSensors();
            last_sensor_ms = now;
        }
    }

    // -------------------------------------------------------
    // STEP 3: HEARTBEAT (every 5000ms)
    // -------------------------------------------------------
    if (tx_hb_pending || (now - last_hb_ms >= HEARTBEAT_INTERVAL_MS)) {
        if (btSerial.available()) {
            tx_hb_pending = true;
        } else {
            guardedSendHeartbeat();
            last_hb_ms = now;
        }
    }

    // -------------------------------------------------------
    // STEP 4: Brief yield for incoming bytes
    // -------------------------------------------------------
    // A tiny delay gives the SoftwareSerial ISR time to capture
    // any bytes that arrived during our processing above.
    // This is especially important after a TX operation.
    delay(1);
}
