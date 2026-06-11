# 02 - بروتوكول JSON للاتصال (JSON Communication Protocol)

## 1. نظرة عامة

الاتصال بين Raspberry Pi و Arduino يتم عبر **سطر واحد من JSON في كل رسالة** (JSON Lines / NDJSON format). كل رسالة تنتهي بـ `\n` وتُحلَّل بشكل مستقل.

```
Pi ──→ Arduino:  {"dir":"F","spd":200}\n
Arduino ──→ Pi:  {"T":25.5,"H":60.0,"G":312,"D":34,"R":5}\n
```

---

## 2. تصنيف الرسائل

### 2.1 أوامر الحركة (Motion Commands)

#### أمر اتجاه (Direction Command)
```json
{"dir": "F", "spd": 200}
```

| الحقل | النوع | القيم الممكنة | الوصف |
|-------|------|--------------|-------|
| `dir` | string | F, B, L, R, FL, FR, BL, BR, SPIN_L, SPIN_R, S | اتجاه الحركة |
| `spd` | int | 0-255 | سرعة الحركة |

**جدول الاتجاهات وتأثيرها على العجلات:**

| الاتجاه | اليسار (FL+RL) | اليمين (FR+RR) | الوصف |
|---------|----------------|----------------|-------|
| F (Forward) | +spd | +spd | أمام |
| B (Backward) | -spd | -spd | خلف |
| L (Left) | -spd | +spd | دوران يسار |
| R (Right) | +spd | -spd | دوران يمين |
| FL | +spd/2 | +spd | أمام يسار |
| FR | +spd | +spd/2 | أمام يمين |
| BL | -spd/2 | -spd | خلف يسار |
| BR | -spd | -spd/2 | خلف يمين |
| SPIN_L | -spd | +spd | دوران بالمكان يسار |
| SPIN_R | +spd | -spd | دوران بالمكان يمين |
| S (Stop) | 0 | 0 | توقف |

#### تحكم مباشر بالمحركات (Direct Motor Control)
```json
{"m1": 200, "m2": -150, "m3": 200, "m4": -150}
```

| الحقل | النوع | المجال | الوصف |
|-------|------|--------|-------|
| `m1` | int | -255..255 | سرعة محرك 1 (منفذ 1) |
| `m2` | int | -255..255 | سرعة محرك 2 (منفذ 2) |
| `m3` | int | -255..255 | سرعة محرك 3 (منفذ 3) |
| `m4` | int | -255..255 | سرعة محرك 4 (منفذ 4) |

> **ملاحظة**: القيمة الموجبة = أمام، السالبة = خلف

#### تحكم بالسيرفو (Servo Control)
```json
{"s1": 90, "s2": 45}
```

| الحقل | النوع | المجال | الوصف |
|-------|------|--------|-------|
| `s1` | int | 0..180 | زاوية السيرفو 1 |
| `s2` | int | 0..180 | زاوية السيرفو 2 |

#### إيقاف طوارئ (Emergency Stop)
```json
{"stop": true}
```

---

### 2.2 أوامر المعايرة (Calibration Commands)

#### تشغيل منفذ واحد للمعايرة
```json
{"cal": "port", "port": 1, "spd": 150, "ms": 700}
```
يشغّل منفذ فيزيائي مفرد بسرعة معينة لمدة محددة ثم يوقفه تلقائياً.

#### اختبار اتجاه عجلة منطقية
```json
{"cal": "dir", "wheel": "FL", "spd": 150, "ms": 700}
```
يشغّل عجلة منطقية (باستخدام الربط والانعكاس المحفوظ) لاختبار الاتجاه.

#### تحريك سيرفو للمعايرة
```json
{"cal": "servo", "id": 1, "angle": 90}
```

---

### 2.3 أوامر الإعدادات (Configuration Commands)

#### قراءة الإعدادات
```json
{"cfg": "get"}
```
**الرد:**
```json
{"cfg": "dump", "port_FL": 1, "port_FR": 2, "port_RL": 3, "port_RR": 4, "inv_FL": 0, "inv_FR": 0, ...}
```

#### تعيين ربط المنافذ (Port Mapping)
```json
{"cfg": "map", "FL": 1, "FR": 2, "RL": 3, "RR": 4}
```
يربط كل عجلة منطقية بمنفذ فيزيائي (1-4) على الشيلد.

#### عكس اتجاه عجلة
```json
{"cfg": "inv", "wheel": "FL", "invert": 1}
```
`invert: 1` يعكس اتجاه الدوران لتلك العجلة.

#### ضبط حدود السيرفو
```json
{"cfg": "servo", "id": 1, "min": 0, "max": 180, "center": 90, "invert": 0}
```

#### حفظ في EEPROM
```json
{"cfg": "save"}
```
**الرد:** `{"cfg": "saved"}`

#### إعادة ضبط المصنع
```json
{"cfg": "reset"}
```
**الرد:** `{"cfg": "reset_done"}`

---

### 2.4 رسائل من Arduino → Pi

#### قراءات الحساسات (كل 1000ms)
```json
{"T": 25.5, "H": 60.0, "G": 312, "D": 34, "R": 5}
```

| الكود | الحساس | الوحدة | المصدر |
|-------|--------|--------|--------|
| T | Temperature (DHT11) | °C | `dht.readTemperature()` |
| H | Humidity (DHT11) | % | `dht.readHumidity()` |
| G | Gas (MQ2) | ppm (raw ADC) | `analogRead(A1)` |
| D | Distance (HC-SR04) | cm | `pulseIn()` * 0.034 / 2 |
| R | Rain | % | `analogRead(A0)` mapped 0-100 |

#### نبضة حياة (Heartbeat) كل 2000ms
```json
{"hb": 1}
```
تُستخدم للتأكد من أن الأردوينو لا يزال يعمل.

#### خطأ
```json
{"err": "json"}
```
يُرسل عند فشل تحليل JSON الوارد.

---

## 3. خوارزمية معالجة JSON في Arduino

```cpp
void processJson(const char *s) {
    StaticJsonDocument<192> doc;
    DeserializationError err = deserializeJson(doc, s);
    
    if (err) {
        btSerial.println(F("{\"err\":\"json\"}"));
        return;
    }
    
    // 1. أوامر الإعدادات (لا تحدّث failsafe timer)
    if (doc.containsKey("cfg")) { handleCfg(doc); return; }
    
    // 2. أوامر المعايرة (لا تحدّث failsafe timer)
    if (doc.containsKey("cal")) { handleCal(doc); return; }
    
    // 3. أوامر الاتجاه
    if (doc.containsKey("dir")) {
        // معالجة dir + spd
        last_cmd_ms = millis();  // تحديث failsafe
        return;
    }
    
    // 4. إيقاف طوارئ
    if (doc.containsKey("stop")) { stopAll(); return; }
    
    // 5. تحكم مباشر بالمحركات والسيرفو
    // m1..m4, s1, s2
}
```

### أولوية المعالجة:
1. `cfg` — إعدادات (أعلى أولوية)
2. `cal` — معايرة
3. `dir` — اتجاه
4. `stop` — إيقاف طوارئ
5. `m1-m4, s1-s2` — تحكم مباشر

---

## 4. خوارزمية القراءة char-by-char

الأردوينو لا يملك `readLine()` مدمج، لذلك يقرأ حرفاً بحرف:

```cpp
char cmd_buffer[128];
uint8_t cmd_index = 0;

while (btSerial.available()) {
    char c = (char)btSerial.read();
    if (c == '\n' || c == '\r') {
        if (cmd_index > 0) {
            cmd_buffer[cmd_index] = '\0';  // إنهاء النص
            processJson(cmd_buffer);        // معالجة
            cmd_index = 0;                  // إعادة تعيين
        }
    } else {
        if (cmd_index < 127) {
            cmd_buffer[cmd_index++] = c;
        }
        // تجاهل الحروف الزائدة (overflow protection)
    }
}
```

**ميزات هذه الخوارزمية:**
- **Non-blocking**: لا توقف `loop()` الرئيسي
- **Overflow protection**: حد أقصى 127 حرف
- **Line-oriented**: تعمل مع أي عدد من الرسائل المتتالية

---

## 5. أحجام StaticJsonDocument

| الاستخدام | الحجم | السبب |
|-----------|-------|-------|
| قراءة أوامر | 192 bytes | كافي لأي أمر وارد |
| قراءات الحساسات | 128 bytes | {"T":25.5,"H":60,"G":312,"D":34,"R":5} |
| dumpConfig | 256 bytes | كل حقول الإعدادات |

> ArduinoJson تستخدم **تخصيص ثابت على الـ Stack** (لا heap) لتجنب Fragmentation.

---

## 6. توجيه الرسائل في Pi (Message Routing)

```python
# bluetooth.py — _data_reader_loop()
def _data_reader_loop(self):
    SENSOR_CODES = {"T", "H", "G", "D", "R", "L", "P"}
    
    while self._running:
        data = self.read_json(timeout=0.5)
        if not isinstance(data, dict):
            continue
        
        if "hb" in data:
            self._last_hb = time.time()          # نبضة حياة
        elif "err" in data:
            logger.warning("Arduino error: %s", data.get("err"))
        elif "cfg" in data:
            self._socketio.emit("calib_config", data)  # معايرة
        else:
            # حساسات — فلترة بالأكواد المعتمدة
            readings = {k: v for k, v in data.items() if k in SENSOR_CODES}
            if readings:
                self._sensor_manager.update(readings)
                self._socketio.emit("sensor_data", readings)
```

---

## 7. أسئلة للمراجعة

1. لماذا حجم `StaticJsonDocument<192>` وليس أكبر؟ ماذا يحدث لو أرسلنا JSON أكبر من 192 بايت؟
2. ما الفرق بين `cfg` و `cal` من حيث تحديث `last_cmd_ms`؟ ولماذا؟
3. كيف يتم التعامل مع الرسائل التالفة (Corrupted JSON)؟
4. لماذا لا يستخدم Arduino `String` class بل `char[]` buffer؟
5. ماذا يحدث لو أرسل Pi رسالتين متتاليتين بدون انتظار؟
