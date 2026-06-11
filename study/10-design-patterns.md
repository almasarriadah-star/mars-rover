# 10 - أنماط التصميم المستخدمة (Design Patterns)

## 1. نظرة عامة

المشروع يستخدم عدة أنماط تصميم (Design Patterns) معروفة في هندسة البرمجيات. فهم هذه الأنماط يساعد في صيانة الكود وتطويره.

---

## 2. نمط المدير/المتحكم (Manager Pattern)

### الوصف:
كل وحدة في المشروع عبارة عن فئة "Manager" تدير مجالاً معيناً:

```python
BluetoothManager   → إدارة الاتصال البلوتوث
CameraManager      → إدارة الكاميرا والبث
SensorManager      → إدارة الحساسات والتسجيل
MotorController    → التحكم بالمحركات
ScenarioManager    → إدارة السيناريوهات
SerialCommunicator  → إدارة الاتصال السيريال
```

### الكود:
```python
# كل Manager يُهيَّأ في app.py
bt_manager = BluetoothManager(config.get("bluetooth", {}))
camera_manager = CameraManager(config.get("camera", {}))
motor_controller = MotorController(bt_manager, socketio)
sensor_manager = SensorManager()
scenario_manager = ScenarioManager(motor_controller, bt_manager, socketio)
```

### المميزات:
- **مسؤولية واحدة**: كل Manager يهتم بمجال واحد
- **تهيئة مركزية**: كل شيء في `app.py`
- **قابلية الاختبار**: يمكن اختبار كل Manager بشكل مستقل

---

## 3. نمط المراقب (Observer Pattern)

### الوصف:
عندما تتغير حالة في النظام، يتم إعلام جميع المهتمين (المتصفح) تلقائياً.

### التطبيق عبر SocketIO:

```python
# Subject (الناشر)
class SensorManager:
    def update(self, readings):
        sensor.value = value
        self._emit_sensor_data(sensor)  # ← إرسال لكل المراقبين

# Observer (المراقب) — في المتصفح
socket.on("sensor_data", (data) => {
    updateDashboard(data);  # تحديث الواجهة
});
```

### أحداث المراقبة في المشروع:

| Subject | Event | Observer |
|---------|-------|----------|
| SensorManager | `sensor_data` | Dashboard charts |
| SensorManager | `sensor_warning` | Alert notifications |
| MotorController | `motor_status` | Speed gauges |
| BluetoothManager | `bluetooth_status` | Connection indicator |
| CameraManager | `camera_frame` | Video display |
| ScenarioManager | `scenario_progress` | Progress bar |

---

## 4. نمط الأوامر (Command Pattern)

### الوصف:
كل طلب من المستخدم يُغلَّف كـ "أمر" مستقل يحتوي على كل المعلومات اللازمة.

### التطبيق في execute_move:

```python
class MotorController:
    DIRECTION_PRESETS = {
        "F":  {"l": 200, "r": 200},
        "B":  {"l": -200, "r": -200},
        "L":  {"l": -150, "r": 150},
        # ...
    }
    
    def execute_move(self, data: dict):
        """تنفيذ أمر حركة — يمكن أن يكون اتجاه أو تحكم مباشر"""
        direction = data.get("direction")
        
        if direction:
            # Command: Direction Preset
            preset = self.DIRECTION_PRESETS[direction]
            # ... apply
        
        # أو Command: Direct Motor Control
        m1 = data.get("m1")
        # ... apply
```

### التطبيق في ScenarioStep:

```python
class ScenarioStep:
    """أمر واحد قابل للتنفيذ"""
    def __init__(self, motors: dict, duration: int):
        self.motors = motors    # WHAT to do
        self.duration = duration  # HOW LONG
```

---

## 5. نمط الاستراتيجية (Strategy Pattern)

### الوصف:
خوارزميات مختلفة لنفس المهمة، قابلة للتبديل.

### التطبيق في مرشحات الكاميرا:

```python
class CameraManager:
    def _apply_filters(self, frame):
        # Strategy: Color Mode
        if self.color_mode == "RGB":
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        elif self.color_mode == "Grayscale":
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        elif self.color_mode == "Binary":
            # threshold strategy
        # BGR = default (no-op)
        
        # Strategy: Edge Detection (on/off)
        if self.edge_detection:
            edges = cv2.Canny(frame, 100, 200)
            # ...
```

**يمكن تغيير الاستراتيجية أثناء التشغيل:**
```javascript
socket.emit("camera_settings", {color_mode: "Grayscale"});
socket.emit("camera_settings", {edge_detection: true});
```

---

## 6. نمط الواجهة (Facade Pattern)

### الوصف:
تقديم واجهة مبسطة لنظام معقد.

### التطبيق في SerialCommunicator:

```python
# بدلاً من:
serial.Serial(port, baudrate, timeout=1, write_timeout=1)
connection.write(data)
connection.readline()

# نستخدم:
comm = SerialCommunicator(port, baudrate)
comm.open()
comm.write_line('{"dir":"F","spd":200}')
data = comm.read_json()
```

### التطبيق في BluetoothManager:

```python
# الواجهة المبسطة:
bt_manager.connect("AA:BB:CC:DD:EE:FF")  # بدلاً من 4 أوامر نظام
bt_manager.send_json({"m1": 200})          # بدلاً من serialize + write
data = bt_manager.read_json()              # بدلاً من read + deserialize
```

---

## 7. نمط التسجيل/السجل (Registry Pattern)

### الوصف:
مكان مركزي لتسجيل والبحث عن الكائنات.

### التطبيق في SensorManager:

```python
class SensorManager:
    def __init__(self):
        self.sensors: dict[str, Sensor] = {}  # Registry
    
    def register_sensor(self, sensor: Sensor):
        self.sensors[sensor.code] = sensor    # Register
    
    def get(self, code: str):
        return self.sensors.get(code.upper())  # Lookup
    
    def remove_sensor(self, code: str):
        del self.sensors[code]                 # Unregister
    
    def get_all(self):
        return [s.to_dict() for s in self.sensors.values()]  # All
```

**المميزات:**
- حساسات ديناميكية: إضافة/إزالة أثناء التشغيل
- بحث سريع: `dict.get(code)` = O(1)
- مركزي: كل الحساسات في مكان واحد

---

## 8. نمط التسلسل/الإلغاء (Serialization Pattern)

### الوصف:
تحويل الكائنات لتمثيل قابل للتخزين والنقل، والعكس.

### التطبيق:

```python
class Sensor:
    def to_dict(self):          # للإرسال عبر WebSocket
        return {"code": self.code, "name": self.name, ...}
    
    def to_config_dict(self):   # للحفظ في JSON file
        return {"code": self.code, "min": self.min_val, ...}
    
    @classmethod
    def from_config_dict(cls, d):  # للتحميل من JSON
        return cls(code=d["code"], ...)

class Scenario:
    def to_dict(self):           # للحفظ
        return {"name": self.name, "steps": [...], ...}
    
    @classmethod
    def from_dict(cls, data):    # للتحميل
        return cls(name=data["name"], ...)
```

---

## 9. نمط القالب (Template Method Pattern)

### الوصف:
تحديد هيكل خوارزمية مع ترك خطوات محددة للفئات الفرعية.

### التطبيق في خط أنابيب المرشحات:

```python
def _apply_filters(self, frame):
    # القالب: خطوات ثابتة الترتيب
    frame = self._apply_color_mode(frame)      # Step 1
    frame = self._apply_edge_detection(frame)  # Step 2
    frame = self._apply_blur(frame)            # Step 3
    frame = self._apply_roi(frame)             # Step 4
    frame = self._apply_resize(frame)          # Step 5
    return frame
```

كل خطوة يمكن تفعيلها/تعطيلها بشكل مستقل.

---

## 10. نمط Deep Merge (دمج عميق)

### الوصف:
دمج إعدادات جديدة مع القديمة بدون فقدان البيانات.

```python
def _deep_merge(base, override):
    """دمج عميق لقاموسين"""
    for key, value in override.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)  # دمج متكرر
        else:
            base[key] = value              # استبدال
```

**مثال:**
```python
base = {"server": {"host": "0.0.0.0", "port": 5000}, "motors": {"speed": 200}}
override = {"server": {"port": 8080}}

result = _deep_merge(base, override)
# {"server": {"host": "0.0.0.0", "port": 8080}, "motors": {"speed": 200}}
# ↑ host محفوظ، port محدّث
```

---

## 11. ملخص الأنماط

| النمط | أين | الفائدة |
|-------|-----|---------|
| Manager | كل الوحدات | مسؤولية واحدة، قابلية اختبار |
| Observer | SocketIO events | تحديثات لحظية |
| Command | MotorController, ScenarioStep | أوامر قابلة للتخزين |
| Strategy | Camera filters | تبديل خوارزميات ديناميكياً |
| Facade | SerialCommunicator, BluetoothManager | تبسيط الواجهات |
| Registry | SensorManager | إدارة كائنات ديناميكية |
| Serialization | Sensor, Scenario | حفظ واسترجاع |
| Template Method | Filter pipeline | ترتيب ثابت مع مرونة |
| Deep Merge | Config update | تحديث جزئي بدون فقدان |

---

## 12. أسئلة للمراجعة

1. أعطِ مثالاً على كيف يسهّل نمط Facade إضافة module جديد.
2. لو أردنا إضافة "Log Manager" جديد، أي نمط نستخدم؟ ولماذا؟
3. كيف يختلف Observer عبر SocketIO عن Observer التقليدي؟
4. لماذا Strategy أفضل من if/elif لكثير من المرشحات؟
