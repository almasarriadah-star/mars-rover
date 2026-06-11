# 05 - خوارزميات إدارة الحساسات (Sensor Management Algorithms)

## 1. نظرة عامة

نظام الحساسات يعمل بثلاث طبقات:

```
┌────────────────────────────────────────────────────────────┐
│  Arduino (Hardware Layer)                                   │
│  • قراءة خام من ADC والمتحولات                              │
│  • تحويل أولي (units, mapping)                              │
│  • إرسال JSON كل 1000ms                                    │
├────────────────────────────────────────────────────────────┤
│  SensorManager (Business Logic Layer)                       │
│  • تسجيل ديناميكي للحساسات                                  │
│  • تتبع التاريخ (History) مع deque                          │
│  • كشف التحذيرات (Threshold Detection)                      │
│  • تحديث WebSocket لحظي                                    │
├────────────────────────────────────────────────────────────┤
│  Web Dashboard (Presentation Layer)                         │
│  • عرض القيم الحالية                                        │
│  • رسوم بيانية حية                                         │
│  • تنبيهات بصرية عند التحذيرات                              │
└────────────────────────────────────────────────────────────┘
```

---

## 2. قراءة الحساسات في Arduino

### 2.1 خوارزمية القراءة

```cpp
void readAndSendSensors() {
    float t = dht.readTemperature();          // DHT11
    float h = dht.readHumidity();             // DHT11
    int   g = analogRead(MQ2_PIN);            // MQ2 Gas
    int   r = constrain(map(analogRead(RAIN_PIN), 0, 1023, 100, 0), 0, 100);  // Rain
    long  d = readUltrasonicCm();             // HC-SR04
    
    StaticJsonDocument<128> doc;
    if (!isnan(t)) doc[F("T")] = round1(t);  // تقريب لعشري واحد
    if (!isnan(h)) doc[F("H")] = round1(h);
    doc[F("G")] = g;
    doc[F("R")] = r;
    if (d > 0) doc[F("D")] = d;
    
    serializeJson(doc, btSerial);
    btSerial.println();
}
```

### 2.2 حساس المطر — خوارزمية التحويل

```cpp
int r = constrain(map(analogRead(RAIN_PIN), 0, 1023, 100, 0), 0, 100);
```

**التحليل:**
```
ADC value: 0-1023 (10-bit ADC في Arduino)
جاف = قيمة ADC عالية → نريده = 0% مطر
مبلل = قيمة ADC منخفضة → نريده = 100% مطر

map(value, 0, 1023, 100, 0):
  ADC=0    → 100% (مبلل جداً)
  ADC=512  → 50%
  ADC=1023 → 0%  (جاف)

constrain(..., 0, 100): ضمان أن القيمة بين 0 و 100
```

### 2.3 حساس المسافة — خوارزمية HC-SR04

```cpp
long readUltrasonicCm() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);     // نبضة 10μs
    digitalWrite(TRIG_PIN, LOW);
    
    unsigned long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);  // timeout 30ms
    if (dur == 0) return 0;     // لا إشارة
    
    return (long)(dur * 0.034 / 2.0);  // تحويل لـ cm
}
```

**الفيزياء:**
```
سرعة الصوت = 340 m/s = 0.034 cm/μs

المسافة = الزمن × السرعة / 2
         = dur(μs) × 0.034(cm/μs) / 2

مثال: dur = 1000μs
المسافة = 1000 × 0.034 / 2 = 17 cm
```

**حدود:**
- `pulseIn` timeout = 30ms → أقصى مسافة = 30000 × 0.034 / 2 = **510 cm**
- HC-SR04 فعلي: **2-400 cm**

### 2.4 تقريب القراءات

```cpp
// تقريب لعشري واحد
(float)((int)(t * 10 + 0.5)) / 10.0

مثال: t = 25.47
25.47 * 10 = 254.7
254.7 + 0.5 = 255.2
(int)255.2 = 255
255 / 10.0 = 25.5

مثال: t = 25.44
25.44 * 10 = 254.4
254.4 + 0.5 = 254.9
(int)254.9 = 254
254 / 10.0 = 25.4
```

---

## 3. فئة Sensor (البيانات والبيانات الوصفية)

```python
class Sensor:
    def __init__(self, code, name, unit, icon, min_val, max_val,
                 warn_high, warn_low, color):
        self.code = code.upper()          # كود الحساس (T, H, G, D, R)
        self.name = name                  # الاسم
        self.unit = unit                  # الوحدة
        self.min_val = float(min_val)     # أقل قيمة متوقعة
        self.max_val = float(max_val)     # أعلى قيمة متوقعة
        self.warn_high = float(warn_high) # عتبة التحذير العلوي
        self.warn_low = float(warn_low)   # عتبة التحذير السفلي
        
        self.value = None                 # القيمة الحالية
        self.timestamp = None             # وقت آخر قراءة
        self.history = deque(maxlen=100)  # سجل آخر 100 قراءة
        self.warning = None               # None | "high" | "low"
```

### لماذا deque؟

```python
from collections import deque

# deque vs list:
# deque(maxlen=100): تلقائياً يحذف الأقدم عند الامتلاء
# list: يحتاج manual trimming

history = deque(maxlen=100)
history.append({"value": 25.5, "timestamp": "..."})
# بعد 101 إضافة: أول عنصر يُحذف تلقائياً ← O(1)
```

---

## 4. SensorManager — التسجيل والتحديث

### 4.1 التسجيل الديناميكي

```python
class SensorManager:
    def register_sensor(self, sensor: Sensor):
        self.sensors[sensor.code] = sensor  # استبدال لو نفس الكود
    
    def register_sensor_from_params(self, code, name, unit, icon, **kwargs):
        sensor = Sensor(code=code, name=name, unit=unit, icon=icon, **kwargs)
        self.register_sensor(sensor)
        return sensor
```

**ميزة التسجيل الديناميكي:**
- يمكن إضافة/إزالة حساسات أثناء التشغيل
- لا حاجة لإعادة تشغيل السيرفر
- كل حساس يُعرَّف بـ **كود حرف واحد** (T, H, G, D, R, L, P)

### 4.2 تحديث القراءات

```python
def update(self, readings: dict):
    """
    readings = {"T": 25.5, "H": 60.0, "G": 312, "D": 34, "R": 5}
    """
    now = datetime.now()
    
    for code, value in readings.items():
        code = code.upper()
        sensor = self.sensors.get(code)
        if not sensor:
            continue  # حساس غير مسجّل → تجاهل
        
        # 1. تحويل لرقم
        value = float(value)
        
        # 2. Clamp إلى النطاق
        value = max(sensor.min_val, min(sensor.max_val, value))
        
        # 3. تحديث الحالة
        sensor.value = value
        sensor.timestamp = now
        sensor.history.append({"value": value, "timestamp": now.isoformat()})
        
        # 4. كشف التحذيرات
        prev_warning = sensor.warning
        sensor.warning = self._check_thresholds(sensor)
        
        # 5. إرسال تحذير لو تغيّرت الحالة
        if sensor.warning != prev_warning:
            self._emit_sensor_warning(sensor, now)
        
        # 6. إرسال بيانات محدّثة
        self._emit_sensor_data(sensor)
```

---

## 5. خوارزمية كشف التحذيرات (Threshold Detection)

```python
@staticmethod
def _check_thresholds(sensor: Sensor):
    """Return "high", "low", or None"""
    if sensor.warn_high is not None and sensor.value >= sensor.warn_high:
        return "high"
    if sensor.warn_low is not None and sensor.value <= sensor.warn_low:
        return "low"
    return None
```

### أمثلة عملية:

| الحساس | warn_low | warn_high | القيمة | التحذير |
|--------|----------|-----------|--------|---------|
| Temperature | -10 | 80 | 25.5 | None |
| Temperature | -10 | 80 | 85.0 | "high" |
| Distance | 10 | None | 5.0 | "low" (قريب جداً!) |
| Gas | None | 300 | 400 | "high" (غاز مرتفع!) |

### منطق التحذيرات المتقدّم:

```python
# إرسال تحذير فقط عند تغيّر الحالة (لا spam)
prev_warning = sensor.warning
sensor.warning = self._check_thresholds(sensor)

if sensor.warning != prev_warning:
    # تحذير جديد: None→high, None→low, high→None, low→None
    self._emit_sensor_warning(sensor, now)
```

---

## 6. بيانات الرسوم البيانية (Chart Data)

```python
def get_chart_data(self, code: str = None, limit: int = 100):
    limit = max(1, min(limit, 100))  # clamp 1-100
    
    if code:
        sensor = self.sensors.get(code.upper())
        return {
            sensor.code: {
                "name": sensor.name,
                "unit": sensor.unit,
                "color": sensor.color,
                "data": list(sensor.history)[-limit:],
            }
        }
    # كل الحساسات
    return {code: {...} for code, sensor in self.sensors.items()}
```

---

## 7. الحساسات الافتراضية

| الكود | الاسم | الوحدة | Range | warn_high | warn_low |
|-------|-------|--------|-------|-----------|----------|
| T | Temperature | °C | -40..125 | 80 | -10 |
| H | Humidity | % | 0..100 | 90 | 10 |
| G | Gas | ppm | 0..5000 | 1000 | — |
| D | Distance | cm | 2..400 | — | 10 |
| L | Light | lux | 0..100000 | 80000 | 5 |
| P | Pressure | hPa | 300..1100 | 1050 | 500 |

---

## 8. الحفظ والاسترجاع (Persistence)

```python
def save_config(self):
    """حفظ بيانات الحساسات الوصفية (بدون القراءات)"""
    data = [s.to_config_dict() for s in self.sensors.values()]
    with open(filepath, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

def load_config(self):
    """تحميل حساسات من ملف JSON"""
    with open(filepath, "r") as f:
        data = json.load(f)
    for entry in data:
        sensor = Sensor.from_config_dict(entry)
        self.register_sensor(sensor)
```

---

## 9. أسئلة للمراجعة

1. لماذا نستخدم `deque(maxlen=100)` بدلاً من `list` للحفاظ على التاريخ؟
2. احسب: كم بايت تستهلك 100 قراءة لحساس واحد؟
3. ما الفرق بين `min_val`/`max_val` و `warn_low`/`warn_high`؟
4. لماذا التحقق `isnan(t)` قبل إرسال درجة الحرارة؟
5. في خوارزمية HC-SR04: لماذا القسمة على 2؟
6. كيف يتعامل النظام مع قراءة سالبة لحساس المسافة؟
