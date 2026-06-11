# 11 - التخييط والتزامن (Threading & Concurrency)

## 1. نظرة عامة

المشروع يستخدم **Python threading** لتنفيذ عدة مهام في نفس الوقت. فهم التزامن ضروري لمنع التعارضات (race conditions) والأقفال الميتة (deadlocks).

```
Main Thread (Flask-SocketIO)
    │
    ├─── Bluetooth Monitor Thread ─── يراقب الاتصال كل 5s
    ├─── Bluetooth Reader Thread ──── يقرأ البيانات كل 0.1s
    ├─── Camera Stream Thread ─────── يبث إطارات كل 1/FPS s
    ├─── Motor Ramp Thread ────────── يدرّج السرعة كل 50ms
    └─── Scenario Thread ──────────── ينفذ خطوات سيناريو
```

---

## 2. الأدوات المستخدمة

### 2.1 threading.Thread

```python
import threading

# إنشاء خيط
thread = threading.Thread(
    target=function_name,     # الدالة التي ستنفذ
    args=(arg1, arg2),        # المعاملات
    daemon=True,              # ينتهي مع البرنامج الرئيسي
    name="descriptive-name"   # اسم للتصحيح
)

# بدء
thread.start()

# انتظار انتهاء
thread.join(timeout=5.0)
```

### 2.2 threading.Lock

```python
lock = threading.Lock()

with lock:         # الحصول على القفل تلقائياً
    # code here    # خيط واحد فقط ينفذ هذا في أي لحظة
# يُحرَّر تلقائياً عند الخروج
```

### 2.3 threading.Event

```python
event = threading.Event()

# الخيط المنفّذ:
while not event.is_set():
    do_work()
    event.wait(timeout=0.05)  # انتظر أو استمر

# خيط التحكم:
event.set()     # إشارة: توقف!
event.clear()   # إعادة تعيين
```

---

## 3. الخيوط في المشروع

### 3.1 خيط مراقبة البلوتوث

```python
def _connection_monitor_loop(self):
    while self._running:
        # فحص حالة الاتصال
        if self.is_connected and not self._serial_comm.is_open():
            self.is_connected = False
            self._emit_status()
        
        # إعادة اتصال تلقائي
        if not self.is_connected and self.auto_reconnect:
            self.auto_connect()
        
        time.sleep(5)  # فحص كل 5 ثواني
```

**دورة الحياة:** تبدأ مع `start_monitor()` وتنتهي مع `stop_monitor()`

### 3.2 خيط قراءة البيانات

```python
def _data_reader_loop(self):
    SENSOR_CODES = {"T", "H", "G", "D", "R", "L", "P"}
    
    while self._running:
        if self.is_connected:
            data = self.read_json(timeout=0.5)
            # توجيه البيانات...
        time.sleep(0.1)  # 10 قراءات/ثانية
```

### 3.3 خيط بث الكاميرا

```python
def _stream_loop(self):
    while self.is_running:
        ret, frame = self.cap.read()
        frame = self._apply_filters(frame)
        # encode + emit
        time.sleep(1.0 / self.fps)  # التحكم بـ FPS
```

### 3.4 خيط تدرج المحركات

```python
def _ramp_worker(self):
    while not self._ramp_stop_event.is_set():
        # حساب الخطوة التالية
        new_speed = _ramp_step(current, target)
        # تطبيق
        send_command()
        # انتظار
        if self._ramp_stop_event.wait(timeout=0.05):
            break
```

### 3.5 خيط السيناريو

```python
def _run_loop(self, scenario):
    while True:  # loop
        for step in scenario.steps:
            if self._stop_event.is_set():
                return
            execute_step(step)
            wait_with_cancel(step.duration)
        if not scenario.loop:
            break
```

---

## 4. مشاكل التزامن والحلول

### 4.1 مشكلة: تعارض قراءة/كتابة السيريال

```
Thread 1 (Writer): write('{"m1":200}')
Thread 2 (Reader): read_line()

بدون حماية: قد يتداخلان → بيانات تالفة!
```

**الحل: Lock في SerialCommunicator**
```python
class SerialCommunicator:
    def __init__(self, port, baudrate):
        self.lock = threading.Lock()
    
    def write_line(self, line):
        with self.lock:  # ← قفل
            self.connection.write(...)
    
    def read_line(self):
        with self.lock:  # ← نفس القفل
            line = self.connection.readline()
```

### 4.2 مشكلة: تعارض حالة المحركات

```
Thread 1 (Ramp): self._m1_speed = 100
Thread 2 (Command): self._m1_speed = 200
Thread 3 (Status): return self._m1_speed  → 100? 200? غير محدد!
```

**الحل: Lock في MotorController**
```python
class MotorController:
    def __init__(self):
        self._lock = threading.Lock()
    
    def set_motor_speed(self, m1=None, m2=None):
        with self._lock:
            self._m1_speed = clamp(m1)
            self._m2_speed = clamp(m2)
    
    def get_state(self):
        with self._lock:
            return {"m1": self._m1_speed, ...}
```

### 4.3 مشكلة: تعارض قائمة السيناريوهات

```
Thread 1 (Run): scenario = self._scenarios[name]
Thread 2 (Delete): del self._scenarios[name]
→ KeyError!
```

**الحل: Lock في ScenarioManager**
```python
class ScenarioManager:
    def __init__(self):
        self._lock = threading.Lock()
    
    def create_scenario(self, name, ...):
        with self._lock:
            self._scenarios[name] = scenario
    
    def delete_scenario(self, name):
        with self._lock:
            del self._scenarios[name]
```

---

## 5. خيوط Daemon

```python
thread = threading.Thread(target=..., daemon=True)
```

**ما معنى Daemon Thread؟**
- ينتهي تلقائياً عندما ينتهي البرنامج الرئيسي
- لا يمنع `sys.exit()` من التنفيذ
- مناسب للخيوط الخلفية (مراقبة، بث، قراءة)

**في المشروع:** كل الخيوط هي daemon threads

---

## 6. الإلغاء الآمن (Graceful Cancellation)

### النمط 1: Event-based (المستخدم في السيناريو و الـ Ramp)

```python
# الخيط المنفّذ
self._stop_event = threading.Event()

while not self._stop_event.is_set():
    do_work()
    self._stop_event.wait(timeout=0.05)  # فحص + انتظار

# الإلغاء
self._stop_event.set()
thread.join(timeout=5.0)
```

### النمط 2: Flag-based (المستخدم في Bluetooth Monitor)

```python
# الخيط المنفّذ
self._running = True

while self._running:
    do_work()
    time.sleep(5)

# الإلغاء
self._running = False
```

---

## 7. مخطط الخيوط الكامل

```
Main Thread (Flask-SocketIO)
│
│  بدء التشغيل:
│  ├── bt_manager.start_monitor(socketio)
│  │   ├── _connection_monitor_thread (daemon)
│  │   └── _data_reader_thread (daemon)
│  │
│  ├── camera_manager.start()
│  │   └── camera_manager.start_stream(socketio)
│  │       └── _stream_thread (daemon)
│  │
│  └── auto_connect (optional)
│       └── auto_connect_thread (daemon)
│
│  أثناء التشغيل:
│  ├── motor_controller.set_motor_speed(ramp=True)
│  │   └── ramp_thread (daemon, مؤقت)
│  │
│  └── scenario_manager.run_scenario(name)
│      └── run_thread (daemon, مؤقت)
│
│  الإيقاف (Ctrl+C):
│  ├── camera_manager.stop()
│  ├── bt_manager.disconnect()
│  └── bt_manager.stop_monitor()
```

---

## 8. أسئلة للمراجعة

1. ما الفرق بين `daemon=True` و `daemon=False`؟
2. لماذا نستخدم `Event` للإلغاء بدلاً من `Thread.terminate()`؟
3. ما Race Condition؟ أعطِ مثالاً من المشروع.
4. احسب: كم خيط يعمل عندما يكون السيناريو والكاميرا والبلوتوث كلهم نشطين؟
5. ماذا يحدث لو Lock اتنين Threads على نفس القفل؟
6. لماذا `Event.wait(timeout=0.05)` أفضل من `time.sleep(0.05)` للإلغاء؟
