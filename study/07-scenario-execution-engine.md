# 07 - محرك تنفيذ السيناريوهات (Scenario Execution Engine)

## 1. نظرة عامة

نظام السيناريوهات يسمح بإنشاء **تسلسلات حركة مبرمجة** مسبقاً وتنفيذها تلقائياً. يشبه مفهوم **Macro** أو **Script** في أنظمة الأتمتة.

```
┌────────────────────────────────────────────────────┐
│  User creates scenario:                            │
│  "patrol" = [                                      │
│    {direction: "F", speed: 80}, duration: 3000ms}, │
│    {direction: "R", speed: 50}, duration: 1500ms}, │
│    {direction: "F", speed: 80}, duration: 3000ms}, │
│    {direction: "L", speed: 50}, duration: 1500ms}, │
│  ], loop: true                                     │
└────────────────────────────────────────────────────┘
          │ save as JSON
          ▼
┌────────────────────────────────────────────────────┐
│  scenarios/patrol.json                             │
└────────────────────────────────────────────────────┘
          │ load + run
          ▼
┌────────────────────────────────────────────────────┐
│  ScenarioManager._run_loop()                       │
│  Background Thread:                                │
│  Step 1: execute_move(F, 80%) → wait 3000ms        │
│  Step 2: execute_move(R, 50%) → wait 1500ms        │
│  Step 3: execute_move(F, 80%) → wait 3000ms        │
│  Step 4: execute_move(L, 50%) → wait 1500ms        │
│  → loop: true → repeat from Step 1                 │
└────────────────────────────────────────────────────┘
```

---

## 2. الهيكل الطبقي (Class Hierarchy)

```
ScenarioStep       ← خطوة واحدة (أوامر + مدة)
Scenario           ← مجموعة خطوات + اسم + وصف + loop
ScenarioManager    ← إدارة CRUD + تنفيذ + حفظ + تحميل
```

---

## 3. فئة ScenarioStep

### البنية:

```python
class ScenarioStep:
    def __init__(self, motors: Dict[str, Any], duration: int):
        # تطبيع المفاتيح (lowercase)
        raw = {k.lower(): v for k, v in motors.items()}
        
        self.motors = {}
        # DC Motors
        for key in ("m1", "m2", "m3", "m4"):
            if key in raw and raw[key] is not None:
                self.motors[key] = raw[key]
        # Servos
        for key in ("s1", "s2"):
            if key in raw and raw[key] is not None:
                self.motors[key] = raw[key]
        # Direction preset
        if "direction" in raw:
            self.motors["direction"] = raw["direction"]
        if "speed" in raw:
            self.motors["speed"] = raw["speed"]
        
        self.duration = duration  # milliseconds
```

### مثال JSON:

```json
{
    "motors": {"direction": "F", "speed": 80},
    "duration": 3000
}
```

### التسلسل (Serialization):

```python
def to_dict(self):
    return {"motors": dict(self.motors), "duration": self.duration}

@classmethod
def from_dict(cls, data):
    return cls(motors=data.get("motors", {}), duration=data.get("duration", 0))
```

---

## 4. فئة Scenario

```python
class Scenario:
    def __init__(self, name, description="", steps=None, loop=False):
        self.name = name           # اسم فريد
        self.description = description
        self.steps = steps or []   # قائمة ScenarioStep
        self.loop = loop           # تكرار مستمر؟
    
    def add_step(self, step: ScenarioStep):
        self.steps.append(step)
    
    def remove_step(self, index: int):
        if 0 <= index < len(self.steps):
            self.steps.pop(index)
```

---

## 5. خوارزمية التنفيذ (Execution Algorithm)

### 5.1 بدء التنفيذ

```python
def run_scenario(self, name: str) -> bool:
    # 1. هل يوجد سيناريو يعمل؟
    if self._running:
        return False  # لا يمكن تشغيل اثنين معاً
    
    # 2. هل السيناريو موجود؟
    scenario = self.get_scenario(name)
    if scenario is None:
        return False
    
    # 3. هل فيه خطوات؟
    if not scenario.steps:
        return False
    
    # 4. تشغيل في خيط خلفي
    self._running = True
    self._stop_event.clear()
    self._run_thread = Thread(target=self._run_loop, args=(scenario,), daemon=True)
    self._run_thread.start()
    return True
```

### 5.2 حلقة التنفيذ الرئيسية

```python
def _run_loop(self, scenario: Scenario):
    total_steps = len(scenario.steps)
    iteration = 0
    
    try:
        while True:  # loop forever
            iteration += 1
            
            for idx, step in enumerate(scenario.steps):
                # ── فحص إيقاف ──
                if self._stop_event.is_set():
                    self._emit_progress(..., status="stopped")
                    return
                
                # ── إرسال تقدم ──
                self._emit_progress(..., status="running")
                
                # ── تنفيذ الأمر ──
                self.motor_controller.execute_move(step.motors)
                
                # ── انتظار المدة (مع فحص إيقاف كل 50ms) ──
                remaining_ms = step.duration
                while remaining_ms > 0:
                    if self._stop_event.is_set():
                        break
                    chunk = min(remaining_ms, 50)
                    time.sleep(chunk / 1000.0)
                    remaining_ms -= chunk
            
            # ── هل نكرر؟ ──
            if not scenario.loop:
                break
            if self._stop_event.is_set():
                break
    
    except Exception as e:
        self._emit_progress(..., status="error", error=str(e))
    finally:
        self._running = False
        self._emit_progress(..., status="completed")
```

### 5.3 خوارزمية الانتظار القابلة للإلغاء

```python
remaining_ms = step.duration
while remaining_ms > 0:
    if self._stop_event.is_set():
        break
    chunk = min(remaining_ms, 50)  # 50ms chunks
    time.sleep(chunk / 1000.0)
    remaining_ms -= chunk
```

**لماذا 50ms chunks؟**
- `time.sleep(3000)` = لا يمكن إلغاؤها! (يستمر 3 ثواني)
- `time.sleep(0.05)` × 60 = يمكن فحص `stop_event` كل 50ms
- **زمن الاستجابة للإيقاف = 50ms كحد أقصى**

---

## 6. إيقاف السيناريو

```python
def stop_scenario(self) -> bool:
    if not self._running:
        return False
    
    # 1. إرسال إشارة إيقاف
    self._stop_event.set()
    
    # 2. إيقاف طوارئ المحركات
    self.motor_controller.emergency_stop()
    
    # 3. انتظار انتهاء الخيط
    if self._run_thread and self._run_thread.is_alive():
        self._run_thread.join(timeout=5.0)
    
    return True
```

---

## 7. نظام الأحداث (Progress Events)

```python
def _emit_progress(self, scenario_name, iteration, step, total_steps, status, error=None):
    payload = {
        "scenario": scenario_name,
        "iteration": iteration,
        "step": step,
        "total_steps": total_steps,
        "status": status,  # "running" | "stopped" | "completed" | "error"
    }
    if error:
        payload["error"] = error
    
    self._socketio.emit("scenario_progress", payload)
```

**أمثلة:**
```json
{"scenario": "patrol", "iteration": 1, "step": 2, "total_steps": 4, "status": "running"}
{"scenario": "patrol", "iteration": 0, "step": 0, "total_steps": 4, "status": "completed"}
{"scenario": "patrol", "iteration": 0, "step": 0, "total_steps": 4, "status": "error", "error": "Motor error"}
```

---

## 8. الحفظ والاسترجاع (Persistence)

### حفظ:

```python
def save_to_file(self, name: str) -> bool:
    scenario = self.get_scenario(name)
    filepath = os.path.join(SCENARIOS_DIR, f"{name}.json")
    with open(filepath, "w", encoding="utf-8") as f:
        json.dump(scenario.to_dict(), f, indent=2, ensure_ascii=False)
```

### تحميل:

```python
def load_from_file(self, name: str):
    filepath = os.path.join(SCENARIOS_DIR, f"{name}.json")
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)
    scenario = Scenario.from_dict(data)
    self._scenarios[name] = scenario
```

### تحميل الكل عند البدء:

```python
def load_all(self) -> int:
    count = 0
    for filename in os.listdir(SCENARIOS_DIR):
        if filename.endswith(".json"):
            name = filename[:-5]  # إزالة .json
            self.load_from_file(name)
            count += 1
    return count
```

---

## 9. مثال كامل: سيناريو "دورية مربعة"

```json
{
    "name": "square_patrol",
    "description": "Move in a square pattern",
    "steps": [
        {"motors": {"direction": "F", "speed": 70}, "duration": 2000},
        {"motors": {"direction": "R", "speed": 50}, "duration": 1000},
        {"motors": {"direction": "F", "speed": 70}, "duration": 2000},
        {"motors": {"direction": "R", "speed": 50}, "duration": 1000},
        {"motors": {"direction": "F", "speed": 70}, "duration": 2000},
        {"motors": {"direction": "R", "speed": 50}, "duration": 1000},
        {"motors": {"direction": "F", "speed": 70}, "duration": 2000},
        {"motors": {"direction": "R", "speed": 50}, "duration": 1000}
    ],
    "loop": true
}
```

**النتيجة:** الروبوت يتحرك في مربع ويعيد نفس المسار باستمرار.

---

## 10. أسئلة للمراجعة

1. لماذا لا يمكن تشغيل سيناريوهين في نفس الوقت؟ وكيف يمكن تعديل الكود لدعم ذلك؟
2. ما الفرق بين `time.sleep(duration/1000)` والـ chunking بـ 50ms؟
3. ماذا يحدث لو `execute_move()` رفع استثناء أثناء السيناريو؟
4. كيف يضمن النظام أن السيناريو لا يبقي المحركات تعمل بعد الإيقاف؟
5. احسب: سيناريو من 4 خطوات × 3 ثواني + loop=true، كم دورة كاملة في دقيقة؟
