# 04 - الاتصال البلوتوث والسيريال (Bluetooth & Serial Communication)

## 1. نظرة عامة

الاتصال بين Raspberry Pi و Arduino Uno يتم عبر **HC-05 Bluetooth Module** باستخدام **RFCOMM Serial Profile**. هذا يحوّل البلوتوث إلى منفذ سيريال افتراضي (`/dev/rfcomm0`).

```
Raspberry Pi                          Arduino Uno
┌──────────────┐    Bluetooth         ┌──────────────┐
│ pyserial     │◄═══════════════════►│ SoftwareSerial│
│ /dev/rfcomm0 │    RFCOMM (9600)     │ (A5=RX, A4=TX)│
└──────────────┘                      └──────────────┘
```

---

## 2. مكدس الاتصال (Communication Stack)

```
┌─────────────────────────────────────┐
│  Application Layer                  │
│  JSON Protocol (Messages)           │
├─────────────────────────────────────┤
│  Transport Layer                    │
│  SerialCommunicator / btSerial      │
│  (read_line / write_line + \\n)     │
├─────────────────────────────────────┤
│  Data Link Layer                    │
│  PySerial (Pi) / HardwareSerial     │
│  UART framing (start/stop bits)     │
├─────────────────────────────────────┤
│  Physical Layer                     │
│  Bluetooth RFCOMM / HC-05           │
│  2.4GHz Radio                       │
└─────────────────────────────────────┘
```

---

## 3. وحدة SerialCommunicator (PySerial Wrapper)

### لماذا نحتاج Wrapper؟

```python
class SerialCommunicator:
    """يغلف PySerial مع قفل خيوط لمنع تعارض القراءة/الكتابة"""
    
    def __init__(self, port: str, baudrate: int = 9600):
        self.port = port
        self.baudrate = baudrate
        self.connection = None
        self.lock = threading.Lock()  # ← حماية من التزامن
```

### المشكلة التي يحلها Lock:
```
Thread 1: write_line('{"m1":200}')  ← يكتب بايتات
Thread 2: read_line()                ← يقرأ بايتات

بدون Lock: قد يتداخل الكتابة مع القراءة → بيانات تالفة!
مع Lock: كل عملية تنتظر حتى تنتهي الأخرى
```

### العمليات الأساسية:

#### الفتح:
```python
def open(self) -> bool:
    self.connection = serial.Serial(
        port=self.port,
        baudrate=self.baudrate,
        timeout=1,           # timeout للقراءة
        write_timeout=1,     # timeout للكتابة
    )
    # تنظيف المخازن المؤقتة
    self.connection.reset_input_buffer()
    self.connection.reset_output_buffer()
```

#### الكتابة:
```python
def write_line(self, line: str) -> bool:
    with self.lock:  # ← قفل
        self.connection.write((line + "\n").encode("utf-8"))
        self.connection.flush()  # إرسال فوري
```

#### القراءة:
```python
def read_line(self, timeout: float = 1.0) -> str:
    with self.lock:  # ← قفل
        old_timeout = self.connection.timeout
        self.connection.timeout = timeout
        line = self.connection.readline()  # يقرأ حتى \n
        self.connection.timeout = old_timeout
        return line.decode("utf-8", errors="ignore").strip()
```

#### قراءة JSON:
```python
def read_json(self, timeout: float = 1.0) -> dict:
    line = self.read_line(timeout)
    if line:
        return json.loads(line)
    return None
```

---

## 4. BluetoothManager — إدارة الاتصال

### 4.1 البحث عن الأجهزة (Scanning)

```python
def scan_devices(self, timeout: int = None) -> list:
    # الطريقة 1: bluetoothctl (الحديثة)
    result = subprocess.run(
        ["bluetoothctl", "--timeout", str(timeout), "scan", "on"],
        capture_output=True, text=True, timeout=timeout + 5
    )
    
    # تحليل المخرجات
    for line in output.split("\n"):
        if "Device" in line:
            # استخراج العنوان والاسم
            # مثال: "Device AA:BB:CC:DD:EE:FF HC-05"
            addr = rest[:addr_end].strip()
            name = rest[addr_end:].strip()
```

### 4.2 الاتصال (Connection Sequence)

```
خطوات الاتصال:
1. تحرير أي اتصال سابق → _release_rfcomm()
2. Pairing → bluetoothctl pair <address>
3. Trust → bluetoothctl trust <address>
4. Bind RFCOMM → rfcomm bind <port> <address> 1
5. فتح Serial → SerialCommunicator.open()
```

```python
def connect(self, address: str = None) -> bool:
    self._release_rfcomm()
    
    # 1. Pairing
    subprocess.run(["bluetoothctl", "pair", address], ...)
    
    # 2. Trust
    subprocess.run(["bluetoothctl", "trust", address], ...)
    
    # 3. Bind RFCOMM
    subprocess.run(["sudo", "rfcomm", "bind", str(self.rfcomm_port), address, "1"], ...)
    
    # 4. فتح Serial
    self._serial_comm = SerialCommunicator(self.serial_port, self.baudrate)
    return self._serial_comm.open()
```

### 4.3 إعادة الاتصال التلقائي (Auto-Reconnect)

```python
def auto_connect_with_retry(self) -> bool:
    for attempt in range(1, self.reconnect_attempts + 1):
        if self.auto_connect():
            return True
        time.sleep(self.reconnect_delay)
    return False
```

**الإعدادات:**
- `reconnect_attempts`: عدد المحاولات (افتراضي: 5)
- `reconnect_delay`: ثواني بين المحاولات (افتراضي: 3)
- أقصى مدة: 5 × 3 = 15 ثانية

---

## 5. خيوط الخلفية (Background Threads)

### 5.1 خيط مراقبة الاتصال

```python
def _connection_monitor_loop(self):
    """يفحص كل 5 ثواني"""
    while self._running:
        # هل الاتصال لا يزال حياً؟
        if self.is_connected and not self._serial_comm.is_open():
            self.is_connected = False
            self._emit_status()
        
        # إعادة اتصال تلقائي
        if not self.is_connected and self.auto_reconnect and self.target_address:
            self.auto_connect()
        
        time.sleep(5)
```

### 5.2 خيط قراءة البيانات

```python
def _data_reader_loop(self):
    """يقرأ البيانات الواردة باستمرار"""
    SENSOR_CODES = {"T", "H", "G", "D", "R", "L", "P"}
    
    while self._running:
        if self.is_connected:
            data = self.read_json(timeout=0.5)
            
            if "hb" in data:
                self._last_hb = time.time()
            elif "err" in data:
                # إرسال خطأ للواجهة
            elif "cfg" in data:
                # إرسال إعدادات معايرة
            else:
                # حساسات — توجيه لـ SensorManager
                readings = {k: v for k, v in data.items() if k in SENSOR_CODES}
                self._sensor_manager.update(readings)
        
        time.sleep(0.1)  # 10 قراءات/ثانية
```

---

## 6. Hardware: HC-05 و مقسم الجهد

### 6.1 لماذا مقسم الجهد؟

```
Arduino A4 = 5V output
HC-05 RX  = 3.3V input ← يتحرق لو وصّلناه مباشر!

الحل: مقسم جهد (Voltage Divider)
```

```
Arduino A4 ──[ 1kΩ ]──┬── HC-05 RX
                       │
                   [ 2kΩ ]
                       │
                      GND

V_out = V_in × R2 / (R1 + R2)
V_out = 5V × 2kΩ / (1kΩ + 2kΩ) = 3.33V ✅
```

### 6.2 SoftwareSerial على Arduino

```cpp
// A5 = RX (يستقبل من HC-05 TX)
// A4 = TX (يرسل إلى HC-05 RX عبر مقسم الجهد)
SoftwareSerial btSerial(A5, A4);
```

**لماذا SoftwareSerial وليس Hardware Serial؟**
- Hardware Serial (D0/D1) محجوز للبرمجة والـ USB Debug
- SoftwareSerial يسمح بالتواصل مع HC-05 على أرجل أخرى

---

## 7. بروتوكول UART (الطبقة المادية)

```
Baud Rate: 9600 bits/second
Data Bits: 8
Parity: None
Stop Bits: 1

كل بايت = 1 start bit + 8 data bits + 1 stop bit = 10 bits
السرعة الفعلية = 9600 / 10 = 960 bytes/second

رسالة JSON نموذجية: {"T":25.5,"H":60.0,"G":312,"D":34,"R":5}
الطول: ~45 bytes
الزمن: 45 / 960 ≈ 47ms
```

---

## 8. معالجة الأخطاء

| الخطأ | السبب | المعالجة |
|-------|-------|----------|
| `TimeoutExpired` | الجهاز لا يستجيب | تجاهل وحاول مجدداً |
| `FileNotFoundError` | `bluetoothctl` غير موجود | Fallback لـ `hcitool` |
| `SerialException` | المنفذ مشغول/غير موجود | Log + إعادة محاولة |
| `JSONDecodeError` | رسالة تالفة | تجاهل (ليست JSON صالحة) |

---

## 9. أسئلة للمراجعة

1. لماذا `flush()` بعد `write()`؟ ماذا يحدث بدونها؟
2. ما الفرق بين `timeout` للقراءة و `write_timeout` للكتابة؟
3. احسب: كم رسالة حساسات يمكن إرسالها في الثانية؟
4. لماذا نستخدم `errors="ignore"` عند decode؟
5. لو أردنا رفع الـ baudrate إلى 115200، ماذا نحتاج نغيّر؟
6. ماذا يحدث لو ماتت عملية Python أثناء وجود بيانات في buffer السيريال؟
