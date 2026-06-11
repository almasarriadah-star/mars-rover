# 09 - Firmware الأردوينو و EEPROM (Arduino Firmware & Persistent Storage)

## 1. نظرة عامة

الـ Firmware المكتوب بلغة C++ يعمل على **Arduino Uno** ويتحكم مباشرة بالعتاد:

```
                    Arduino Uno
┌────────────────────────────────────────────────┐
│  setup()                                       │
│  │ Serial.begin(9600)    // USB debug          │
│  │ btSerial.begin(9600)  // HC-05              │
│  │ dht.begin()           // DHT11              │
│  │ servo.attach(9/10)    // Servos             │
│  │ loadConfig()          // EEPROM             │
│  │ stopAll()             // محركات متوقفة      │
│  │ LED blinks            // مؤشر الحالة        │
│  ▼                                             │
│  loop() — يعمل بلا توقف                         │
│  │ 1. اقرأ btSerial char-by-char               │
│  │ 2. processJson() عند سطر كامل               │
│  │ 3. Failsafe: auto-stop بعد 800ms            │
│  │ 4. قراءة حساسات كل 1000ms                   │
│  │ 5. Heartbeat كل 2000ms                      │
│  └──► repeat                                   │
└────────────────────────────────────────────────┘
```

---

## 2. خريطة الأرجل (Pin Map)

```cpp
#define DHT_PIN   2    // DHT11 Data
#define RAIN_PIN  A0   // Rain Sensor Analog
#define MQ2_PIN   A1   // MQ2 Gas Analog
#define ECHO_PIN  A3   // HC-SR04 Echo (input)
#define TRIG_PIN  A2   // HC-SR04 Trig (output)
// A4 = btSerial TX → HC-05 RX (via voltage divider)
// A5 = btSerial RX ← HC-05 TX
#define LED_PIN   13   // Status LED

// Shield يحجز تلقائياً:
// D3, D4, D5, D6, D7, D8 → Motor control (L293D)
// D9  → Servo 1 (SER1)
// D10 → Servo 2 (SER2)
// D11, D12 → 74HC595 shift register
```

### جدول الأرجل:

| الرجل | الجهاز | الاتجاه | ملاحظة |
|-------|--------|---------|--------|
| D2 | DHT11 | INPUT | + مقاومة pull-up 10kΩ |
| D3-D8 | L293D Shield | — | محجوز تلقائياً |
| D9 | Servo 1 | OUTPUT | SER1 header |
| D10 | Servo 2 | OUTPUT | SER2 header |
| D11-D12 | 74HC595 | — | محجوز تلقائياً |
| D13 | LED | OUTPUT | مدمج باللوحة |
| A0 | Rain | INPUT | تناظري |
| A1 | MQ2 | INPUT | تناظري |
| A2 | HC-SR04 ECHO | INPUT | رقمي |
| A3 | HC-SR04 TRIG | OUTPUT | رقمي |
| A4 | HC-05 RX (TX out) | OUTPUT | عبر مقسم جهد |
| A5 | HC-05 TX (RX in) | INPUT | مباشر |

---

## 3. بنية EEPROM Config

### 3.1 الـ Struct

```cpp
#define CFG_MAGIC   0xA5
#define CFG_VERSION 1
#define CFG_ADDR    0

struct RobotConfig {
    uint8_t magic;        // 0xA5 — تأكيد صحة البيانات
    uint8_t version;      // 1 — رقم نسخة البنية
    
    // ربط المنافذ (1-4 → M1-M4)
    uint8_t port_FL;
    uint8_t port_FR;
    uint8_t port_RL;
    uint8_t port_RR;
    
    // عكس الاتجاه (0=عادي, 1=معكوس)
    uint8_t inv_FL;
    uint8_t inv_FR;
    uint8_t inv_RL;
    uint8_t inv_RR;
    
    // حدود السيرفو 1
    uint8_t s1_min;
    uint8_t s1_max;
    uint8_t s1_center;
    uint8_t s1_invert;
    
    // حدود السيرفو 2
    uint8_t s2_min;
    uint8_t s2_max;
    uint8_t s2_center;
    uint8_t s2_invert;
    
    // عام
    uint8_t default_speed;
    
    uint8_t checksum;     // XOR لكل البايتات السابقة
};
```

### 3.2 حجم الـ Struct

```
حقل              نوع        بايتات
─────────────────────────────────
magic            uint8_t    1
version          uint8_t    1
port_FL..RR      uint8_t×4  4
inv_FL..RR       uint8_t×4  4
s1_min..invert   uint8_t×4  4
s2_min..invert   uint8_t×4  4
default_speed    uint8_t    1
checksum         uint8_t    1
─────────────────────────────────
المجموع                     24 bytes
```

> Arduino Uno EEPROM = **1024 bytes** ← نستخدم 24 فقط (2.3%)

---

## 4. خوارزمية Checksum

```cpp
uint8_t calcChecksum(const RobotConfig &c) {
    const uint8_t *p = (const uint8_t *)&c;
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(RobotConfig) - 1; i++) {
        x ^= p[i];  // XOR accumulation
    }
    return x;
}
```

**كيف يعمل XOR Checksum:**

```
Byte 0: 0xA5 → x = 0x00 ^ 0xA5 = 0xA5
Byte 1: 0x01 → x = 0xA5 ^ 0x01 = 0xA4
Byte 2: 0x01 → x = 0xA4 ^ 0x01 = 0xA5
...
Byte 22: ... → x = final checksum
```

**التحقق:**
```cpp
bool loadConfig() {
    EEPROM.get(CFG_ADDR, cfg);
    
    if (cfg.magic != CFG_MAGIC ||
        cfg.version != CFG_VERSION ||
        cfg.checksum != calcChecksum(cfg)) {
        // بيانات فاسدة أو غير موجودة → defaults
        loadDefaults();
        saveConfig();
        return false;
    }
    return true;
}
```

---

## 5. خوارزمية Failsafe

```cpp
unsigned long last_cmd_ms = 0;
bool motors_active = false;

void loop() {
    // ...
    unsigned long now = millis();
    
    if (motors_active && (now - last_cmd_ms > 800UL)) {
        stopAll();  // إيقاف تلقائي
    }
}
```

### مخطط التوقيت:

```
t=0ms:    أمر حركة ← last_cmd_ms = 0
t=100ms:  أمر حركة ← last_cmd_ms = 100
t=200ms:  أمر حركة ← last_cmd_ms = 200
t=800ms:  (فحص) 200 + 800 = 1000 > 800 → لا توقف
t=900ms:  (فحص) 200 + 800 = 1000 ≤ 900? لا
t=1001ms: (فحص) 1001 - 200 = 801 > 800 → STOP! ✓
```

**ملاحظة مهمة:** `cfg` و `cal` أوامر لا تحدّث `last_cmd_ms` لأنها ليست أوامر حركة.

---

## 6. مؤشر LED للبدء (Boot Indicator)

```cpp
void setup() {
    bool loaded = loadConfig();
    
    // ... initialization ...
    
    int blinks = loaded ? 2 : 3;
    for (int i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(100);
        digitalWrite(LED_PIN, LOW);  delay(100);
    }
}
```

| وميض | المعنى |
|------|--------|
| 2 مرات | تم تحميل إعدادات محفوظة من EEPROM |
| 3 مرات | تم كتابة إعدادات افتراضية (جديد/فاسد) |

---

## 7. ArduinoJson — تخصيص الذاكرة الثابت

```cpp
StaticJsonDocument<192> doc;
DeserializationError err = deserializeJson(doc, s);
```

**لماذا Static وليس Dynamic؟**

```
StaticJsonDocument<N>:
- يُخصص على Stack (سريع)
- حجم معروف وقت الترجمة
- لا fragmentation
- مناسب للأنظمة المحدودة (Arduino 2KB RAM)

DynamicJsonDocument:
- يُخصص على Heap (أبطأ)
- يحتاج malloc/free
-可能导致 fragmentation
- غير مستخدم هنا
```

---

## 8. دورة حياة النظام الكاملة

```
Power On
    │
    ▼
setup()
    │ Serial.begin(9600)
    │ btSerial.begin(9600)
    │ dht.begin()
    │ servo1.attach(9) / servo2.attach(10)
    │ loadConfig() ← EEPROM
    │ stopAll()
    │ writeServo(center)
    │ LED blinks (2 or 3)
    │
    ▼
loop() ────────────────────────────────────────┐
    │                                          │
    ├─ 1. Read btSerial → processJson()        │
    │     ├─ cfg commands                      │
    │     ├─ cal commands                      │
    │     ├─ dir → driveSides()                │
    │     ├─ stop → stopAll()                  │
    │     └─ m1-m4, s1-s2 → direct control     │
    │                                          │
    ├─ 2. Failsafe check (800ms)               │
    │                                          │
    ├─ 3. Sensor telemetry (1000ms)            │
    │     ├─ DHT11: T, H                       │
    │     ├─ MQ2: G                            │
    │     ├─ Rain: R                           │
    │     └─ HC-SR04: D                        │
    │                                          │
    ├─ 4. Heartbeat (2000ms)                   │
    │     └─ {"hb":1}                          │
    │                                          │
    └─ ──── repeat ────────────────────────────┘
```

---

## 9. أسئلة للمراجعة

1. لماذا نستخدم XOR checksum وليس CRC أو MD5؟
2. ماذا يحدث لو كانت EEPROM فارغة تماماً (Arduino جديد)؟
3. احسب: كم دورة `loop()` تنفذ في الثانية؟ (تلميح: يعتمد على وقت القراءات)
4. لماذا `millis()` وليس `delay()` لإدارة التوقيت؟
5. لو أردت إضافة حساس جديد، ماذا تحتاج تغيير في Firmware؟
6. ما الفرق بين `EEPROM.put()` و `EEPROM.write()`؟
