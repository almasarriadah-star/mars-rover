# 01 - هندسة النظام الكاملة (System Architecture)

## 1. نظرة عامة

مشروع **Mars Rover** هو نظام تحكم بروبوت متكامل يعتمد على بنية **Master-Slave** بين جهازين:

| الجهاز | الدور | اللغة | نظام التشغيل |
|--------|-------|-------|-------------|
| **Raspberry Pi** | الخادم (Master) | Python 3 | Linux |
| **Arduino Uno** | المتحكم (Slave) | C/C++ | Bare-metal |

الاتصال بين الجهازين يتم عبر **Bluetooth (HC-05)** باستخدام **Serial UART** وبروتوكول **JSON**.

---

## 2. المخطط العام للنظام

```
┌─────────────────────────────────────────────────────────────────────┐
│                        المتصفح (Web Browser)                        │
│                   index.html / settings.html                        │
└──────────────────────┬──────────────────────────────────────────────┘
                       │ HTTP / WebSocket (SocketIO)
                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     Raspberry Pi Server                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Flask    │  │ SocketIO │  │ Camera   │  │ Config Manager   │   │
│  │ (HTTP)   │  │ (WS)     │  │ (OpenCV) │  │ (YAML)           │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Bluetooth│  │ Motor    │  │ Sensor   │  │ Scenario Manager │   │
│  │ Manager  │  │ Controller│ │ Manager  │  │                  │   │
│  └────┬─────┘  └──────────┘  └──────────┘  └──────────────────┘   │
│       │ SerialCommunicator (PySerial)                                │
└───────┼─────────────────────────────────────────────────────────────┘
        │ Bluetooth RFCOMM (Serial Profile)
        ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Arduino Uno + L293D Shield                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ JSON     │  │ Motor    │  │ Sensor   │  │ EEPROM Config    │   │
│  │ Parser   │  │ Driver   │  │ Reader   │  │ (Calibration)    │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ 4x DC    │  │ 2x Servo │  │ DHT11    │  │ HC-SR04          │   │
│  │ Motors   │  │ Motors   │  │ MQ2/Rain │  │ Ultrasonic       │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. طبقات النظام (Layered Architecture)

### الطبقة الأولى: واجهة المستخدم (Presentation Layer)
- **التقنية**: HTML/CSS/JavaScript + Socket.IO Client
- **الملفات**: `templates/index.html`, `templates/settings.html`
- **الوظيفة**: لوحة تحكم ويب تفاعلية، بث كاميرا مباشر، رسوم بيانية للحساسات

### الطبقة الثانية: خادم الويب (Web Server Layer)
- **التقنية**: Flask + Flask-SocketIO + Flask-CORS
- **الملف الرئيسي**: `app.py`
- **الوظيفة**: 
  - تقديم صفحات HTML عبر HTTP
  - معالجة WebSocket Events للتحكم اللحظي
  - API endpoints لجلب الحالة والإعدادات

### الطبقة الثالثة: منطق الأعمال (Business Logic Layer)
- **الوحدات** (مجلد `modules/`):
  - `bluetooth.py` — إدارة اتصال HC-05
  - `motors.py` — تحكم بالمحركات مع Ramp و Presets
  - `sensors.py` — إدارة حساسات ديناميكية مع تاريخ وتحذيرات
  - `camera.py` — بث فيديو OpenCV مع فلاتر
  - `scenarios.py` — تسلسل حركات مع حفظ/استرجاع
  - `serial_comm.py` — تغليف PySerial مع Thread Safety

### الطبقة الرابعة: البرمجيات الثابتة (Firmware Layer)
- **التقنية**: Arduino C++ مع AFMotor + ArduinoJson + DHT + EEPROM
- **الملف**: `arduino/robot_controller/robot_controller.ino`
- **الوظيفة**: قراءة حساسات، تحريك محركات، معايرة EEPROM، failsafe

---

## 4. تدفق البيانات (Data Flow)

### 4.1 تدفق أوامر الحركة
```
User Click → WebSocket "move" → app.py handle_move()
→ MotorController.execute_move(data)
→ BluetoothManager.send_json(payload)
→ SerialCommunicator.write_line(json_string)
═══ Bluetooth RFCOMM ═══
→ Arduino btSerial.read() → processJson()
→ driveSides(left, right)
→ runWheel(port, invert, speed)
→ AF_DCMotor.setSpeed() + .run()
```

### 4.2 تدفق بيانات الحساسات
```
Arduino loop() كل 1000ms → readAndSendSensors()
→ DHT.readTemperature/Humidity() + analogRead() + readUltrasonicCm()
→ StaticJsonDocument → serializeJson(btSerial)
═══ Bluetooth RFCOMM ═══
→ SerialCommunicator.read_json()
→ BluetoothManager._data_reader_loop()
→ SensorManager.update(readings)
→ SocketIO.emit("sensor_data")
→ Web Dashboard تحديث مباشر
```

### 4.3 تدفق بث الكاميرا
```
CameraManager._stream_loop() كل 1/FPS ثانية
→ cv2.VideoCapture.read() → frame (numpy array)
→ _apply_filters(frame) → processed frame
→ cv2.imencode(".jpg") → JPEG bytes
→ base64.b64encode() → string
→ SocketIO.emit("camera_frame", {data: encoded})
→ <img src="data:image/jpeg;base64,...">
```

---

## 5. التقنيات والمكتبات المستخدمة

### 5.1 جانب Python (Raspberry Pi)

| المكتبة | الإصدار | الوظيفة |
|---------|---------|---------|
| Flask | 3.1.3 | إطار عمل ويب خفيف |
| Flask-SocketIO | 5.6.1 | دعم WebSocket للاتصال اللحظي |
| Flask-CORS | 6.0.2 | السماح بطلبات من أصل مختلف |
| pyserial | 3.5 | الاتصال السيريال عبر Bluetooth |
| bleak | 3.0.2 | البلوتوث低能耗 BLE |
| opencv-python | 4.13 | معالجة الصور والفيديو |
| numpy | 2.4 | عمليات المصفوفات |
| Pillow | 12.2 | معالجة الصور |
| PyYAML | 6.0.3 | قراءة/كتابة إعدادات YAML |

### 5.2 جانب Arduino

| المكتبة | الوظيفة |
|---------|---------|
| AFMotor | التحكم بشيلد L293D (4 محركات DC) |
| Servo | التحكم بمحركات السيرفو |
| SoftwareSerial | اتصال سيريال على أرجل غير RX/TX |
| ArduinoJson | تحليل وبناء JSON |
| DHT | قراءة حساس DHT11 |
| EEPROM | حفظ الإعدادات بشكل دائم |

---

## 6. مبدأ الفصل بين المسؤوليات (Separation of Concerns)

كل وحدة في المشروع مسؤولة عن مهمة واحدة محددة:

```
app.py              → التوجيه (Routing) والربط بين الوحدات فقط
bluetooth.py        → الاتصال البلوتوث والبحث والإعادة
serial_comm.py      → القراءة/الكتابة السيريال مع قفل خيوط
motors.py           → منطق الحركة و Ramping و Presets
sensors.py          → تسجيل وتحديث وتحذيرات الحساسات
camera.py           → التقاط وبث ومعالجة الصور
scenarios.py        → تسلسل وتنفيذ السيناريوهات
robot_controller.ino → التحكم المباشر بالعتاد
```

---

## 7. مفهوم التهيئة الديناميكية (Dynamic Configuration)

النظام يستخدم **config.yaml** كمرجع مركزي للإعدادات:

- **التحميل عند البدء**: `load_config()` يقرأ الملف مرة واحدة
- **التحديث أثناء التشغيل**: `_deep_merge()` يدمج الإعدادات الجديدة مع القديمة
- **التطبيق الفوري**: `_apply_config()` يحدث الوحدات بدون إعادة تشغيل
- **الحفظ المستمر**: `save_config()` يكتب YAML فوراً

```python
def _deep_merge(base, override):
    """دمج عميق — يحافظ على القيم غير المعدّلة"""
    for key, value in override.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value
```

---

## 8. أسئلة للمراجعة

1. ما الفائدة من فصل SerialCommunicator عن BluetoothManager؟
2. لماذا استخدمنا SocketIO بدلاً من HTTP العادي للتحكم بالحركة؟
3. كيف يضمن النظام أن الإعدادات لا تُفقد عند إعادة التشغيل؟
4. ما الفرق بين DIRECTION_PRESETS في Pi و driveSides في Arduino؟
5. ارسم مخطط تدفق البيانات لأمر الطوارئ (Emergency Stop).
