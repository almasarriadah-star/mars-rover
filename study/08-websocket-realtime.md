# 08 - WebSocket والاتصال اللحظي (Real-time Communication)

## 1. نظرة عامة

المشروع يستخدم **Flask-SocketIO** لإنشاء اتصال ثنائي الاتجاه (bidirectional) بين الخادم والمتصفح. هذا يسمح بتحديثات **لحظية** بدون إعادة تحميل الصفحة.

```
┌─────────────┐                      ┌──────────────┐
│   Browser   │◄──── WebSocket ────►│  Flask-SocketIO│
│  (Client)   │   (persistent TCP)   │   (Server)    │
└─────────────┘                      └──────────────┘
    │                                      │
    │ emit("move", {dir:"F"})             │ emit("sensor_data", {...})
    │ ──────────────────────────►         │ ◄──────────────────────────
    │                                      │
    │ emit("camera_start")                │ emit("camera_frame", {data:...})
    │ ──────────────────────────►         │ ◄──────────────────────────
```

---

## 2. لماذا WebSocket وليس HTTP؟

| المعيار | HTTP | WebSocket |
|---------|------|-----------|
| الاتجاه | Client → Server فقط | ثنائي الاتجاه |
| الاتصال | يُفتح ويُغلق لكل طلب | مستمر (persistent) |
| الكفاءة | overhead كبير (headers) | overhead صغير (frames) |
| زمن الاستجابة | 50-200ms | 1-10ms |
| مناسب لـ | تحميل صفحات | بث مباشر، تحكم لحظي |

**في مشروعنا:**
- تحكم بالحركة → يحتاج زمن استجابة قليل → **WebSocket**
- بث كاميرا → يحتاج تدفق مستمر → **WebSocket**
- بيانات حساسات → تحديث كل ثانية → **WebSocket**

---

## 3. إعداد SocketIO في Flask

```python
from flask_socketio import SocketIO, emit
from flask_cors import CORS

app = Flask(__name__)
CORS(app)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")
```

### المعاملات:
- `cors_allowed_origins="*"`: السماح من أي أصل
- `async_mode="threading"`: استخدام Python threading (ليس gevent/eventlet)

---

## 4. جدول الأحداث (Events Reference)

### 4.1 أحداث من Client → Server

| الحدث | البيانات | الوظيفة |
|-------|----------|---------|
| `connect` | — | عميل جديد اتصل |
| `disconnect` | — | عميل انقطع |
| `move` | `{direction: "F", speed: 80}` | أمر حركة |
| `stop` | — | إيقاف طوارئ |
| `servo_control` | `{servo: "S1", angle: 90}` | تحكم بسيرفو |
| `motor_speed` | `{M1: 200, M2: 150}` | تغيير سرعة |
| `bluetooth_scan` | — | بحث بلوتوث |
| `bluetooth_connect` | `{address: "..."}` | اتصال بلوتوث |
| `bluetooth_disconnect` | — | قطع اتصال |
| `camera_start` | — | تشغيل كاميرا |
| `camera_stop` | — | إيقاف كاميرا |
| `camera_settings` | `{...settings}` | تحديث إعدادات |
| `camera_capture` | — | التقاط صورة |
| `camera_record_start` | — | بدء تسجيل |
| `camera_record_stop` | — | إيقاف تسجيل |
| `sensor_add` | `{code, name, unit, ...}` | إضافة حساس |
| `sensor_remove` | `{code: "T"}` | إزالة حساس |
| `sensor_edit` | `{code, name, ...}` | تعديل حساس |
| `scenario_save` | `{name, steps, loop}` | حفظ سيناريو |
| `scenario_run` | `{name: "patrol"}` | تشغيل سيناريو |
| `scenario_stop` | — | إيقاف سيناريو |
| `scenario_delete` | `{name: "patrol"}` | حذف سيناريو |
| `scenario_list` | — | قائمة السيناريوهات |
| `save_settings` | `{...config}` | حفظ إعدادات |
| `get_settings` | — | جلب إعدادات |
| `export_settings` | — | تصدير إعدادات |
| `import_settings` | `{...config}` | استيراد إعدادات |
| `reset_settings` | — | إعادة ضبط |

### 4.2 أحداث من Server → Client

| الحدث | البيانات | مصدر الإرسال |
|-------|----------|-------------|
| `bluetooth_status` | `{connected, address, ...}` | BluetoothManager |
| `bluetooth_devices` | `[{name, address}]` | scan_devices() |
| `motor_status` | `{m1, m2, m3, m4, s1, s2}` | MotorController |
| `sensor_data` | `{T: 25.5, H: 60, ...}` | SensorManager |
| `sensor_warning` | `{code, warning, value, ...}` | Threshold check |
| `sensor_list` | `[{code, name, ...}]` | _emit_sensor_list() |
| `camera_frame` | `{data: "base64..."}` | CameraManager |
| `camera_settings_updated` | `{...settings}` | update_settings() |
| `photo_captured` | `{filename, url}` | capture_photo() |
| `scenario_progress` | `{scenario, step, status, ...}` | ScenarioManager |
| `scenarios_list` | `["patrol", "scan"]` | list_scenarios() |
| `calib_config` | `{cfg: "dump", ...}` | Arduino EEPROM |
| `settings_loaded` | `{...config}` | get/save settings |
| `settings_exported` | `{...config}` | export |
| `log` | `{level, message}` | رسائل سجل |

---

## 5. معالجة اتصال عميل جديد

```python
@socketio.on("connect")
def handle_connect():
    # إرسال الحالة الحالية فوراً
    emit("bluetooth_status", bt_manager.get_status())
    emit("motor_status", motor_controller.get_state())
    emit("sensor_data", sensor_manager.get_all())
```

**المنطق:** عندما يفتح المستخدم الصفحة، يحتاج معرفة الحالة الحالية فوراً:
- هل البلوتوث متصل؟
- ما سرعة المحركات؟
- ما آخر قراءات الحساسات؟

---

## 6. أنماط الإرسال (Emit Patterns)

### 6.1 emit() — إرسال للعميل الحالي فقط
```python
emit("log", {"level": "info", "message": "Camera started"})
# يصل فقط للعميل الذي أرسل الحدث
```

### 6.2 socketio.emit() — إرسال لجميع العملاء
```python
socketio.emit("sensor_data", readings)
# يصل لكل المتصلين
```

### مثال على الاستخدام:

```python
# إرسال لأمر معين لعميل واحد
emit("log", {"message": "Connected!"})

# إرسال بيانات حساسات للكل
socketio.emit("sensor_data", readings)

# إرسال حالة المحرك للكل
socketio.emit("motor_status", motor_controller.get_state())
```

---

## 7. بث الكاميرا (Camera Streaming)

```
Browser                    Server
  │                          │
  │ camera_start             │
  │ ──────────────────────► │
  │                          │ start camera
  │                          │ start stream thread
  │                          │
  │         camera_frame     │
  │ ◄────────────────────── │ (each frame)
  │         camera_frame     │
  │ ◄────────────────────── │ (each frame)
  │         camera_frame     │
  │ ◄────────────────────── │ (each frame)
  │         ...              │
  │                          │
  │ camera_stop              │
  │ ──────────────────────► │
  │                          │ stop camera
```

**في المتصفح:**
```javascript
socket.on("camera_frame", (data) => {
    img.src = "data:image/jpeg;base64," + data.data;
});
```

---

## 8. بيانات الحساسات اللحظية

```
Arduino ──JSON──► Pi ──SocketIO──► Browser
   (1s)          (real-time)      (live chart)

كل ثانية:
Arduino: {"T":25.5,"H":60.0,"G":312,"D":34,"R":5}
    ↓
BluetoothManager._data_reader_loop()
    ↓
SensorManager.update({"T":25.5, "H":60, "G":312, "D":34, "R":5})
    ↓
socketio.emit("sensor_data", {all sensors})
    ↓
Browser: تحديث القيم + إضافة نقطة للرسم البياني
```

---

## 9. HTTP API Endpoints (جانب REST)

| Endpoint | الطريقة | الوظيفة |
|----------|---------|---------|
| `/` | GET | الصفحة الرئيسية |
| `/settings` | GET | صفحة الإعدادات |
| `/api/status` | GET | حالة النظام الكاملة |
| `/api/config` | GET | جلب الإعدادات |
| `/api/config` | POST | تحديث الإعدادات |
| `/api/command` | POST | إرسال أمر مباشر |
| `/api/sensors/history/<code>` | GET | تاريخ حساس |
| `/photos/<filename>` | GET | خدمة صور |
| `/recordings/<filename>` | GET | خدمة تسجيلات |

### مثال: `/api/status`

```json
{
    "bluetooth": {"connected": true, "address": "AA:BB:CC:DD:EE:FF"},
    "camera": {"running": true, "mode": "RGB", "resolution": "640x480"},
    "motors": {"m1": 0, "m2": 0, "m3": 0, "m4": 0, "s1": 90, "s2": 90},
    "sensors": [
        {"code": "T", "name": "Temperature", "value": 25.5, "unit": "°C"},
        {"code": "H", "name": "Humidity", "value": 60.0, "unit": "%"}
    ],
    "scenarios": ["patrol", "square"],
    "uptime": 3600
}
```

---

## 10. أسئلة للمراجعة

1. ما الفرق بين `emit()` و `socketio.emit()`؟ متى نستخدم كل واحدة؟
2. لماذا نرسل الحالة الحالية عند `connect`؟ ماذا لو لم نفعل؟
3. احسب: كم حجم `camera_frame` event في الثانية عند 24fps؟
4. ماذا يحدث لو فتح مستخدمان الصفحة وكل منهما أرسل `move` مختلف؟
5. لماذا `async_mode="threading"` وليس `"gevent"`؟
