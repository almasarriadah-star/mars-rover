# 12 - توصيلات العتاد والحساسات (Hardware Wiring & Sensors)

## 1. نظرة عامة

هذا الملف يشرح كل توصيلة في المشروع، لماذا وُضعت، وكيف تعمل فيزيائياً.

### المكونات:
- Arduino Uno
- Adafruit Motor Shield v1 (2× L293D + 74HC595)
- HC-05 Bluetooth Module
- DHT11 (حرارة + رطوبة)
- MQ2 (حساس غاز)
- HC-SR04 (موجات فوق صوتية / مسافة)
- Rain Sensor (حساس مطر)
- 4× DC Motors
- 2× Servo Motors

---

## 2. الأرجل المحجوزة بواسطة الشيلد

عند تركيب **Adafruit Motor Shield v1** فوق الأردوينو، تُحجز هذه الأرجل تلقائياً:

```
D3, D5, D6  → PWM للمحركات (ENA/ENB على L293D)
D4, D7      → اتجاه المحركات (IN1-IN4 على L293D)
D8          → Latch (74HC595 shift register)
D9, D10     → Servo headers (SER1, SER2)
D11, D12    → Data/Clock (74HC595 shift register)

المحجوز: D3, D4, D5, D6, D7, D8, D9, D10, D11, D12
المتاح: D2, D13, A0, A1, A2, A3, A4, A5
```

---

## 3. كل توصيلة بالتفصيل

### 3.1 HC-05 Bluetooth

```
Arduino A5 (RX) ←──── HC-05 TX     (مباشر: 3.3V كافية لـ HIGH)
Arduino A4 (TX) ────→ HC-05 RX     (عبر مقسم جهد 1kΩ/2kΩ)
Arduino 5V     ────── HC-05 VCC
Arduino GND    ────── HC-05 GND
```

#### مقسم الجهد (Voltage Divider):

```
A4 (5V) ──[1kΩ]──┬── HC-05 RX (3.3V max)
                   │
               [2kΩ]
                   │
                  GND

V_out = 5V × 2kΩ / (1kΩ + 2kΩ) = 3.33V ✅
```

**لماذا؟** رجل RX في HC-05 تتحمل 3.3V فقط. توصيل 5V مباشر = تلف!

#### SoftwareSerial:
```cpp
SoftwareSerial btSerial(A5, A4);  // RX=A5, TX=A4
```

### 3.2 DHT11 (حرارة + رطوبة)

```
DHT11 VCC  → 5V
DHT11 DATA → D2 (+ مقاومة pull-up 10kΩ بين DATA و VCC)
DHT11 GND  → GND
```

#### مقاومة Pull-up:
```
5V ──[10kΩ]──┬── DHT11 DATA ──→ D2
              │
             (pull-up: تضبط الخط لـ HIGH عندما لا يرسل الحساس)
```

**كيف يعمل DHT11:**
1. الأردوينو يرسل إشارة START (LOW لـ 18ms)
2. DHT11 يرد بإشارة响应
3. يرسل 40 bit: رطوبة عالي + رطوبة منخفض + حرارة عالي + حرارة منخفض + checksum
4. كل bit: 50μs LOW + 26-28μs HIGH = "0" أو 70μs HIGH = "1"

### 3.3 MQ2 (حساس الغاز)

```
MQ2 VCC → 5V (مباشر — يسحب تيار للتسخين)
MQ2 GND → GND
MQ2 AO  → A1 (تناظري)
MQ2 DO  → غير موصول
```

**ملاحظات:**
- MQ2 يحتاج **20-60 ثانية تسخين** قبل قراءات مستقرة
- القيمة: 0-1023 (ADC 10-bit)
- الغازات المكتشفة: LPG, Smoke, Alcohol, Methane, Hydrogen

### 3.4 HC-SR04 (المسافة بالموجات فوق الصوتية)

```
HC-SR04 VCC  → 5V
HC-SR04 TRIG → A3 (خرج رقمي)
HC-SR04 ECHO → A2 (دخل رقمي)
HC-SR04 GND  → GND
```

**خوارزمية القياس:**

```
1. أرسل نبضة HIGH لـ 10μs على TRIG
2. HC-SR04 يرسل 8 نبضات ultrasonic عند 40kHz
3. ECHO يصبح HIGH
4. ECHO يعود LOW عند استقبال الصدى
5. pulseIn() يقيس مدة HIGH
6. المسافة = المدة × 0.034 / 2

مثال:
المدة = 580μs
المسافة = 580 × 0.034 / 2 = 9.86 cm
```

**حدود:**
- أقل مسافة: ~2 cm
- أقصى مسافة: ~400 cm
- زاوية الكشف: ~15°

### 3.5 حساس المطر (Rain Sensor)

```
Rain VCC → 5V
Rain GND → GND
Rain AO  → A0 (تناظري)
Rain DO  → غير موصول
```

**تحويل القراءة:**
```cpp
int raw = analogRead(RAIN_PIN);  // 0-1023
int rain_percent = constrain(map(raw, 0, 1023, 100, 0), 0, 100);
// جاف = ADC عالي → 0%
// مبلل = ADC منخفض → 100%
```

### 3.6 المحركات DC (4 عجلات)

```
Motor 1 → Shield M1 (D3/PWM + D4/DIR)
Motor 2 → Shield M2 (D5/PWM + D7/DIR via 74HC595)
Motor 3 → Shield M3 (D6/PWM + D4/DIR via 74HC595)
Motor 4 → Shield M4 (D5/PWM + D7/DIR via 74HC595)
```

**ملاحظة:** أي محرك في أي منفذ — يُحدَّد بالمعايرة (EEPROM mapping)

### 3.7 محركات السيرفو (2)

```
Servo 1 → Shield SER1 (D9)
Servo 2 → Shield SER2 (D10)
```

**كيف يعمل Servo:**
- إشارة PWM كل 20ms
- عرض النبضة يحدد الزاوية:
  - 1ms = 0°
  - 1.5ms = 90°
  - 2ms = 180°

---

## 4. التغذية (Power)

```
┌─────────────────────────────────────────────┐
│                                             │
│  بطارية 6-12V ──→ Shield Ext Power         │
│  (المحركات)      (انزع جمبر PWR)            │
│                                             │
│  BEC 5-6V ──→ Servo Power                   │
│  (السيرفو)   (يمنع Brownout)                │
│                                             │
│  USB/5V ──→ Arduino + Sensors + HC-05       │
│  (المنطق)                                │
│                                             │
│  ⚠️ GND مشترك إلزامي بين كل المصادر       │
│                                             │
└─────────────────────────────────────────────┘
```

### لماذا تغذية منفصلة؟

```
محرك DC يسحب: 200-500mA × 4 = 0.8-2A
سيرفو يسحب: 500mA-1A تحت حمل
الأردوينو يعطي: 500mA عبر USB فقط

لو كل شيء من USB → Brownout → Reset → 🤖💀
```

---

## 5. ملخص التوصيلات

```
A0  ← Rain Sensor (AO)
A1  ← MQ2 Gas (AO)
A2  ← HC-SR04 ECHO
A3  → HC-SR04 TRIG
A4  → HC-05 RX (عبر 1kΩ/2kΩ voltage divider)
A5  ← HC-05 TX (مباشر)
D2  ↔ DHT11 DATA (+ pull-up 10kΩ)
D9  → Servo 1 (SER1 header)
D10 → Servo 2 (SER2 header)
D13 → Status LED (مدمج)
M1-M4 → 4× DC Motors

الكل: 5V + GND مشترك
```

---

## 6. أسئلة للمراجعة

1. لماذا نستخدم SoftwareSerial وليس Hardware Serial للبلوتوث؟
2. احسب: ما الجهد على رجل HC-05 RX لو R1=1.5kΩ و R2=3kΩ؟
3. ماذا يحدث لو وصلنا HC-05 RX مباشرة بـ 5V بدون مقسم جهد؟
4. لماذا MQ2 يحتاج 20-60 ثانية قبل قراءات صحيحة؟
5. احسب المسافة لو pulseIn أعطى 1160μs.
6. لماذا مقاومة pull-up على DHT11 وليس pull-down؟
