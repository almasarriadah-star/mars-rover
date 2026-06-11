/*
 * robot_controller.ino
 * Mars Rover — Arduino Uno + L293D Motor Shield Firmware
 *
 * Hardware:
 *   - Adafruit Motor Shield v1 (2x L293D + 74HC595)
 *   - 4x DC motors on M1..M4
 *   - 2x Servo on SER1(D9) / SER2(D10)
 *   - HC-05 Bluetooth on SoftwareSerial(A5=RX, A4=TX)
 *   - DHT11 on D2
 *   - MQ2  on A1
 *   - Rain sensor on A0
 *   - HC-SR04 TRIG=A3, ECHO=A2
 *   - Status LED on D13
 *
 * JSON Protocol:
 *   Receive (from Pi/app):
 *     {"dir":"F","spd":200}           direction + speed
 *     {"m1":200,"m2":-150}            direct port control (-255..255)
 *     {"s1":90,"s2":45}               servo angles (0..180)
 *     {"stop":true}                   emergency stop
 *     {"cfg":"get"}                   dump config
 *     {"cfg":"map","FL":1,...}        set motor port mapping
 *     {"cfg":"inv","wheel":"FL","invert":1}  set invert flag
 *     {"cfg":"servo","id":1,...}      set servo limits
 *     {"cfg":"save"}                  save to EEPROM
 *     {"cfg":"reset"}                 reset to defaults
 *     {"cal":"port","port":1,"spd":150,"ms":700}
 *     {"cal":"dir","wheel":"FL","spd":150,"ms":700}
 *     {"cal":"servo","id":1,"angle":90}
 *   Send (to Pi/app):
 *     {"T":25.5,"H":60.0,"G":312,"D":34,"R":5}   sensors every 1s
 *     {"hb":1}                                      heartbeat every 2s
 *     {"err":"json"}                                parse error
 *     {"cfg":"dump",...}                            config dump
 *     {"cfg":"saved"} / {"cfg":"reset_done"}
 *
 * Shield pin reservation (DO NOT use):
 *   D3,D4,D5,D6,D7,D8,D9,D10,D11,D12
 * Free digital: D2, D13
 * Free analog:  A0,A1,A2,A3,A4,A5
 */

// ===== LIBRARIES =====
#include <AFMotor.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>   // 6.x — StaticJsonDocument
#include <DHT.h>
#include <EEPROM.h>

// ===== PIN MAP =====
// Shield auto-reserves: D3,D4,D5,D6,D7,D8,D9,D10,D11,D12
// HC-05: SoftwareSerial btSerial(A5, A4)
//   HC-05 TX → A5 (Arduino RX)
//   HC-05 RX → A4 (Arduino TX, via 1k/2k voltage divider)
#define DHT_PIN   2
#define DHT_TYPE  DHT11
#define RAIN_PIN  A0
#define MQ2_PIN   A1
#define ECHO_PIN  A3
#define TRIG_PIN  A2
// A4 = btSerial TX  (→ HC-05 RX via voltage divider)
// A5 = btSerial RX  (← HC-05 TX)
#define LED_PIN   13

// ===== EEPROM CONFIG =====
#define CFG_MAGIC   0xA5
#define CFG_VERSION 1
#define CFG_ADDR    0

struct RobotConfig {
    uint8_t magic;
    uint8_t version;
    // Motor port mapping: which shield port (1..4) drives each logical wheel
    uint8_t port_FL;   // Front-Left
    uint8_t port_FR;   // Front-Right
    uint8_t port_RL;   // Rear-Left
    uint8_t port_RR;   // Rear-Right
    // Direction invert per wheel (0=normal, 1=invert)
    uint8_t inv_FL;
    uint8_t inv_FR;
    uint8_t inv_RL;
    uint8_t inv_RR;
    // Servo 1
    uint8_t s1_min;
    uint8_t s1_max;
    uint8_t s1_center;
    uint8_t s1_invert;
    // Servo 2
    uint8_t s2_min;
    uint8_t s2_max;
    uint8_t s2_center;
    uint8_t s2_invert;
    // General
    uint8_t default_speed;
    uint8_t checksum;   // XOR of all bytes before this field
};

RobotConfig cfg;

// ===== OBJECTS =====
SoftwareSerial btSerial(A5, A4);  // RX=A5, TX=A4

DHT dht(DHT_PIN, DHT_TYPE);

// Motors array indexed 0..3 → ports 1..4
AF_DCMotor motors[4] = {
    AF_DCMotor(1),
    AF_DCMotor(2),
    AF_DCMotor(3),
    AF_DCMotor(4)
};

Servo servo1;
Servo servo2;

// ===== STATE =====
unsigned long last_cmd_ms    = 0;
unsigned long last_sensor_ms = 0;
unsigned long last_hb_ms     = 0;
bool motors_active           = false;   // for failsafe: only stop if running

char cmd_buffer[128];
uint8_t cmd_index = 0;

// ===== EEPROM FUNCTIONS =====

uint8_t calcChecksum(const RobotConfig &c) {
    const uint8_t *p = (const uint8_t *)&c;
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(RobotConfig) - 1; i++) {
        x ^= p[i];
    }
    return x;
}

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
    cfg.checksum = 0;  // will be set in saveConfig
}

void saveConfig() {
    cfg.magic    = CFG_MAGIC;
    cfg.version  = CFG_VERSION;
    cfg.checksum = calcChecksum(cfg);
    EEPROM.put(CFG_ADDR, cfg);  // only writes changed bytes
}

// Returns true if valid config was loaded, false if defaults were written
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

// ===== MOTOR FUNCTIONS =====

// Run a physical port (1..4) at signed speed (-255..255)
void runPort(uint8_t port, int speed) {
    if (port < 1 || port > 4) return;
    speed = constrain(speed, -255, 255);
    AF_DCMotor &m = motors[port - 1];
    if (speed > 0) {
        m.setSpeed((uint8_t)speed);
        m.run(FORWARD);
    } else if (speed < 0) {
        m.setSpeed((uint8_t)(-speed));
        m.run(BACKWARD);
    } else {
        m.setSpeed(0);
        m.run(RELEASE);
    }
}

// Run a logical wheel using its mapped port and invert flag
void runWheel(uint8_t port, uint8_t invert, int speed) {
    runPort(port, invert ? -speed : speed);
}

// Stop all 4 motor ports
void stopAll() {
    for (uint8_t i = 1; i <= 4; i++) runPort(i, 0);
    motors_active = false;
}

// Drive left and right sides at given speeds (-255..255)
void driveSides(int left, int right) {
    runWheel(cfg.port_FL, cfg.inv_FL, left);
    runWheel(cfg.port_RL, cfg.inv_RL, left);
    runWheel(cfg.port_FR, cfg.inv_FR, right);
    runWheel(cfg.port_RR, cfg.inv_RR, right);
    motors_active = (left != 0 || right != 0);
}

// ===== SERVO FUNCTIONS =====

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

// ===== ULTRASONIC =====

long readUltrasonicCm() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    unsigned long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
    if (dur == 0) return 0;
    return (long)(dur * 0.034 / 2.0);
}

// ===== SENSOR TELEMETRY =====

void readAndSendSensors() {
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
    serializeJson(doc, btSerial);
    btSerial.println();
}

// ===== HANDLE CFG COMMAND =====

void handleCfg(StaticJsonDocument<192> &doc) {
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
        btSerial.println(F("{\"cfg\":\"saved\"}"));
    }
    else if (!strcmp(cmd, "reset")) {
        loadDefaults();
        saveConfig();
        btSerial.println(F("{\"cfg\":\"reset_done\"}"));
    }
}

// ===== HANDLE CAL COMMAND =====

void handleCal(StaticJsonDocument<192> &doc) {
    const char *what = doc["cal"];
    if (!what) return;

    int spd = doc.containsKey("spd") ? (int)doc["spd"] : 150;
    int ms  = doc.containsKey("ms")  ? (int)doc["ms"]  : 700;

    if (!strcmp(what, "port")) {
        // Run a single physical port for ms milliseconds then stop
        uint8_t port = doc.containsKey("port") ? (uint8_t)(int)doc["port"] : 0;
        if (port >= 1 && port <= 4) {
            runPort(port, spd);
            delay(ms);
            runPort(port, 0);
        }
    }
    else if (!strcmp(what, "dir")) {
        // Run a logical wheel (using its mapped port+invert) for ms ms then stop
        const char *w = doc["wheel"];
        if (!w) return;
        uint8_t port = 0, inv = 0;
        if      (!strcmp(w, "FL")) { port = cfg.port_FL; inv = cfg.inv_FL; }
        else if (!strcmp(w, "FR")) { port = cfg.port_FR; inv = cfg.inv_FR; }
        else if (!strcmp(w, "RL")) { port = cfg.port_RL; inv = cfg.inv_RL; }
        else if (!strcmp(w, "RR")) { port = cfg.port_RR; inv = cfg.inv_RR; }
        if (port >= 1 && port <= 4) {
            runWheel(port, inv, spd);
            delay(ms);
            runWheel(port, inv, 0);
        }
    }
    else if (!strcmp(what, "servo")) {
        uint8_t id = doc.containsKey("id") ? (uint8_t)(int)doc["id"] : 1;
        int angle  = doc.containsKey("angle") ? (int)doc["angle"] : 90;
        if (id == 1) writeServo1(angle);
        else         writeServo2(angle);
    }
}

// ===== MAIN JSON PROCESSOR =====

void processJson(const char *s) {
    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, s);
    if (err) {
        btSerial.println(F("{\"err\":\"json\"}"));
        Serial.print(F("JSON err: "));
        Serial.println(err.c_str());
        return;
    }

    // --- Config commands (do not update failsafe timer) ---
    if (doc.containsKey("cfg")) {
        handleCfg(doc);
        return;
    }

    // --- Calibration commands (do not update failsafe timer) ---
    if (doc.containsKey("cal")) {
        handleCal(doc);
        return;
    }

    // --- Direction command ---
    if (doc.containsKey("dir")) {
        const char *d = doc["dir"];
        int spd = doc.containsKey("spd") ? (int)doc["spd"] : (int)cfg.default_speed;
        spd = constrain(spd, 0, 255);

        if      (!strcmp(d, "F"))      driveSides( spd,      spd);
        else if (!strcmp(d, "B"))      driveSides(-spd,     -spd);
        else if (!strcmp(d, "L"))      driveSides(-spd,      spd);
        else if (!strcmp(d, "R"))      driveSides( spd,     -spd);
        else if (!strcmp(d, "FL"))     driveSides( spd / 2,  spd);
        else if (!strcmp(d, "FR"))     driveSides( spd,      spd / 2);
        else if (!strcmp(d, "BL"))     driveSides(-spd / 2, -spd);
        else if (!strcmp(d, "BR"))     driveSides(-spd,     -spd / 2);
        else if (!strcmp(d, "SPIN_L")) driveSides(-spd,      spd);
        else if (!strcmp(d, "SPIN_R")) driveSides( spd,     -spd);
        else if (!strcmp(d, "S"))      stopAll();

        last_cmd_ms = millis();
        return;
    }

    // --- Emergency stop ---
    if (doc.containsKey("stop") && (bool)doc["stop"] == true) {
        stopAll();
        last_cmd_ms = millis();
        return;
    }

    // --- Direct motor/servo control ---
    bool moved = false;

    if (doc.containsKey("m1")) { runPort(1, (int)doc["m1"]); moved = true; }
    if (doc.containsKey("m2")) { runPort(2, (int)doc["m2"]); moved = true; }
    if (doc.containsKey("m3")) { runPort(3, (int)doc["m3"]); moved = true; }
    if (doc.containsKey("m4")) { runPort(4, (int)doc["m4"]); moved = true; }

    if (moved) {
        motors_active = true;
        last_cmd_ms   = millis();
    }

    if (doc.containsKey("s1")) writeServo1((int)doc["s1"]);
    if (doc.containsKey("s2")) writeServo2((int)doc["s2"]);
}

// ===== SETUP =====

void setup() {
    Serial.begin(9600);     // USB debug only
    btSerial.begin(9600);   // HC-05 Bluetooth

    dht.begin();

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(LED_PIN,  OUTPUT);

    servo1.attach(9);   // Shield SER1
    servo2.attach(10);  // Shield SER2

    bool loaded = loadConfig();

    // Place all motors in stopped state
    stopAll();

    // Servos to configured center positions
    writeServo1(cfg.s1_center);
    writeServo2(cfg.s2_center);

    // Blink LED: 2 blinks if config loaded, 3 blinks if defaults were written
    int blinks = loaded ? 2 : 3;
    for (int i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(100);
        digitalWrite(LED_PIN, LOW);  delay(100);
    }

    last_cmd_ms    = millis();
    last_sensor_ms = millis();
    last_hb_ms     = millis();

    Serial.println(F("Boot OK"));
}

// ===== LOOP =====

void loop() {
    // --- 1. Read btSerial char-by-char, process complete lines ---
    while (btSerial.available()) {
        char c = (char)btSerial.read();
        if (c == '\n' || c == '\r') {
            if (cmd_index > 0) {
                cmd_buffer[cmd_index] = '\0';
                processJson(cmd_buffer);
                cmd_index = 0;
            }
        } else {
            if (cmd_index < 127) {
                cmd_buffer[cmd_index++] = c;
            }
            // Overflow: silently discard character (buffer too small would corrupt JSON)
        }
    }

    unsigned long now = millis();

    // --- 2. Failsafe: stop motors if no motion command for 800ms ---
    if (motors_active && (now - last_cmd_ms > 800UL)) {
        stopAll();
    }

    // --- 3. Sensor telemetry every 1000ms ---
    if (now - last_sensor_ms > 1000UL) {
        readAndSendSensors();
        last_sensor_ms = now;
    }

    // --- 4. Heartbeat every 2000ms ---
    if (now - last_hb_ms > 2000UL) {
        btSerial.println(F("{\"hb\":1}"));
        last_hb_ms = now;
    }
}
