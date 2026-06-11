# 03 - خوارزميات التحكم بالمحركات (Motor Control Algorithms)

## 1. نظرة عامة

نظام التحكم بالمحركات يعمل على **طبقتين**:

```
┌───────────────────────────────────────────┐
│  Raspberry Pi — MotorController (Python)  │
│  • Direction Presets (F, B, L, R, ...)    │
│  • Speed Scaling (0-100% → -255..255)     │
│  • Ramping (تدرج سلس في السرعة)            │
│  • Side-based model (left/right)          │
└─────────────┬─────────────────────────────┘
              │ JSON: {"m1":200, "m2":-150, ...}
              ▼
┌───────────────────────────────────────────┐
│  Arduino — Motor Driver (C++)             │
│  • Port Mapping (FL→port 1, FR→port 2)    │
│  • Direction Invert (per wheel)           │
│  • Servo Limits (min/max/invert/center)   │
│  • Failsafe (auto-stop after 800ms)       │
│  • AF_DCMotor / Servo control             │
└───────────────────────────────────────────┘
```

---

## 2. نموذج القيادة بالجانبين (Side-Based Driving Model)

الروبوت يستخدم **4 عجلات** منظمة كـ **جانب يسار + جانب يمين**:

```
            مقدّمة الروبوت
        ┌──────────────────────┐
   FL ──┤ أمامي يسار  أمامي يمين ├── FR
        │                      │
   RL ──┤ خلفي يسار    خلفي يمين ├── RR
        └──────────────────────┘
```

**القاعدة الأساسية:**
- **الجانب الأيسر** (FL + RL): يتلقى نفس السرعة `left`
- **الجانب الأيمن** (FR + RR): يتلقى نفس السرعة `right`

---

## 3. خوارزمية DIRECTION_PRESETS (الجانب Pi)

```python
DIRECTION_PRESETS = {
    "F":      {"l": 200, "r": 200},    # أمام
    "B":      {"l": -200, "r": -200},   # خلف
    "L":      {"l": -150, "r": 150},    # يسار (دوران)
    "R":      {"l": 150, "r": -150},    # يمين (دوران)
    "S":      {"l": 0, "r": 0},         # توقف
    "FL":     {"l": 100, "r": 200},     # أمام-يسار (انعطاف)
    "FR":     {"l": 200, "r": 100},     # أمام-يمين (انعطاف)
    "BL":     {"l": -100, "r": -200},   # خلف-يسار
    "BR":     {"l": -200, "r": -100},   # خلف-يمين
    "SPIN_L": {"l": -200, "r": 200},    # دوران بالمكان يسار
    "SPIN_R": {"l": 200, "r": -200},    # دوران بالمكان يمين
}
```

### خوارزمية تطبيق الاتجاه:

```python
def execute_move(self, data):
    direction = data.get("direction", "").upper()
    preset = DIRECTION_PRESETS[direction]
    
    # مقياس السرعة (0-100%)
    speed_scale = max(0, min(100, int(data.get("speed", 100)))) / 100.0
    
    # تطبيق المقياس
    left  = int(preset["l"] * speed_scale)
    right = int(preset["r"] * speed_scale)
    
    # العجلات الخلفية تطابق الأمامية
    m1 = clamp(left)    # FL
    m2 = clamp(right)   # FR
    m3 = m1             # RL = FL
    m4 = m2             # RR = FR
```

### أمثلة عملية:

| الأمر | Speed | FL | FR | RL | RR | الحركة |
|-------|-------|-----|-----|-----|-----|--------|
| F, 100% | 200 | 200 | 200 | 200 | أمام مستقيم |
| F, 50% | 100 | 100 | 100 | 100 | أمام بطيء |
| R, 100% | 150 | -150 | 150 | -150 | دوران يمين |
| FL, 100% | 100 | 200 | 100 | 200 | انعطاف يسار |
| SPIN_L, 100% | -200 | 200 | -200 | 200 | دوران بالمكان |

---

## 4. خوارزمية التدرج السلس (Speed Ramping)

التدرج يمنع **التيار العالي** عند البدء المفاجئ ويحمي المحركات والتروس.

### المبدأ:

```
السرعة الحالية: 0
السرعة الهدف:  200
الخطوة:        +10 كل 50ms

الزمن:  0ms  50ms  100ms  ...  1000ms
السرعة:  0    10    20    ...   200
```

### الخوارزمية:

```python
RAMP_STEP = 10    # التغيير في كل خطوة
RAMP_DELAY = 0.05  # 50ms بين الخطوات

def _ramp_worker(self):
    while not stopped:
        cur_m1 = self._m1_speed
        cur_m2 = self._m2_speed
        
        new_m1 = _ramp_step(cur_m1, target_m1)  # اقترب خطوة واحدة
        new_m2 = _ramp_step(cur_m2, target_m2)
        
        # تطبيق وإرسال
        self._m1_speed = new_m1
        self._m3_speed = new_m1  # mirror left
        self._m2_speed = new_m2
        self._m4_speed = new_m2  # mirror right
        
        send_command()
        
        if new_m1 == target_m1 and new_m2 == target_m2:
            break  # وصلنا الهدف
        
        wait(RAMP_DELAY)

@staticmethod
def _ramp_step(current, target):
    diff = target - current
    if diff == 0:
        return current
    step = RAMP_STEP if diff > 0 else -RAMP_STEP
    new_val = current + step
    # منع التجاوز
    if (step > 0 and new_val > target) or (step < 0 and new_val < target):
        return target
    return new_val
```

### حساب زمن التدرج:

```
المدة = |الهدف - الحالي| / RAMP_STEP * RAMP_DELAY
مثال: |200 - 0| / 10 * 0.05 = 1.0 ثانية
```

---

## 5. خوارزمية Port Mapping في Arduino

### المشكلة:
أي محرك يمكن توصيله بأي منفذ (M1-M4) على الشيلد. كيف نعرف أي منفذ يقابل أي عجلة؟

### الحل: جدول ربط في EEPROM

```cpp
struct RobotConfig {
    uint8_t port_FL;   // أي منفذ (1-4) يقابل أمامي يسار
    uint8_t port_FR;   // أي منفذ (1-4) يقابل أمامي يمين
    uint8_t port_RL;   // أي منفذ (1-4) يقابل خلفي يسار
    uint8_t port_RR;   // أي منفذ (1-4) يقابل خلفي يمين
    uint8_t inv_FL;    // هل يجب عكس اتجاه هذه العجلة؟
    uint8_t inv_FR;
    uint8_t inv_RL;
    uint8_t inv_RR;
};
```

### خوارزمية التشغيل:

```cpp
void runWheel(uint8_t port, uint8_t invert, int speed) {
    runPort(port, invert ? -speed : speed);
}

void driveSides(int left, int right) {
    // الجانب الأيسر
    runWheel(cfg.port_FL, cfg.inv_FL, left);   // أمامي يسار
    runWheel(cfg.port_RL, cfg.inv_RL, left);   // خلفي يسار
    // الجانب الأيمن
    runWheel(cfg.port_FR, cfg.inv_FR, right);  // أمامي يمين
    runWheel(cfg.port_RR, cfg.inv_RR, right);  // خلفي يمين
}

void runPort(uint8_t port, int speed) {
    speed = constrain(speed, -255, 255);
    AF_DCMotor &m = motors[port - 1];
    if (speed > 0) {
        m.setSpeed(speed);
        m.run(FORWARD);
    } else if (speed < 0) {
        m.setSpeed(-speed);
        m.run(BACKWARD);
    } else {
        m.setSpeed(0);
        m.run(RELEASE);
    }
}
```

---

## 6. خوارزمية تحديد حدود السيرفو

```cpp
int applyServoLimits(int angle, uint8_t mn, uint8_t mx, uint8_t inv) {
    if (inv) angle = 180 - angle;           // عكس الزاوية
    return constrain(angle, mn, mx);        // حدود min/max
}
```

**مثال عملي:**
- سيرفو مثبّت ميكانيكياً يتحرك فقط بين 30° و 150°
- `cfg.s1_min = 30`, `cfg.s1_max = 150`
- طلب `angle = 180` → `constrain(180, 30, 150)` → **150°**
- طلب `angle = 0` → `constrain(0, 30, 150)` → **30°**

---

## 7. خوارزمية Failsafe (الحماية الآلية)

```cpp
// في loop() الرئيسي
unsigned long now = millis();

if (motors_active && (now - last_cmd_ms > 800UL)) {
    stopAll();  // إيقاف كل المحركات
}
```

**المنطق:**
1. `last_cmd_ms` يُحدَّث عند كل أمر حركة
2. إذا مرت **800ms** بدون أمر جديد ← **إيقاف تلقائي**
3. يحمي من انقطاع الاتصال أثناء الحركة

---

## 8. الإيقاف الطارئ (Emergency Stop)

```python
# Pi side
def emergency_stop(self):
    self._cancel_ramp()  # إلغاء أي ramp جاري
    
    self._m1_speed = 0
    self._m2_speed = 0
    self._m3_speed = 0
    self._m4_speed = 0
    self._s1_angle = 90   # مركز السيرفو
    self._s2_angle = 90
    
    send_command(m1=0, m2=0, m3=0, m4=0, s1=90, s2=90)
```

---

## 9. خوارزمية Clamping (الحدود)

```python
# DC Motors: سرعة بين -255 و 255
@classmethod
def _clamp_dc(cls, value: int) -> int:
    return max(-255, min(255, int(value)))

# Servo: زاوية بين 0 و 180
@classmethod
def _clamp_servo(cls, value: int) -> int:
    return max(0, min(180, int(value)))
```

---

## 10. أسئلة للمراجعة

1. لماذا FL و RL يأخذان نفس السرعة؟ ماذا يحدث لو أعطيناهما سرعات مختلفة؟
2. احسب زمن الـ ramp من السرعة -200 إلى +200.
3. ما الفرق بين SPIN_L و L؟
4. لماذا نحتاج `inv_FL`؟ أعطِ مثالاً عملياً.
5. لو أرسل Pi أمر حركة ثم انقطعت الكهرباء عن البلوتوث، ماذا يحدث بعد 800ms؟
6. لماذا `constrain(angle, mn, mx)` وليس `min(max(angle, mn), mx)`؟ (تلميح: كلاهما نفس الشيء في C++)
