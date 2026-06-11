# 06 - معالجة الصور بـ OpenCV (Camera & Image Processing)

## 1. نظرة عامة

وحدة الكاميرا تستخدم **OpenCV** لالتقاط الفيديو من كاميرا Raspberry Pi وتطبيق **مرشحات متعددة** ثم بثه عبر **WebSocket** كصور JPEG مشفرة بـ Base64.

```
Camera → Capture Frame → Apply Filters → Encode JPEG → Base64 → WebSocket → Browser
         (cv2)          (pipeline)       (imencode)    (b64)   (SocketIO)  (<img>)
```

---

## 2. تهيئة الكاميرا (Camera Initialization)

```python
def start(self) -> bool:
    self.cap = cv2.VideoCapture(self.camera_index)  # فتح الكاميرا
    
    # ضبط الخصائص
    self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.frame_width)   # 640
    self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.frame_height)  # 480
    self.cap.set(cv2.CAP_PROP_FPS, self.fps)                    # 24
    self.cap.set(cv2.CAP_PROP_BRIGHTNESS, self.brightness / 100.0)
    self.cap.set(cv2.CAP_PROP_CONTRAST, self.contrast / 100.0)
    self.cap.set(cv2.CAP_PROP_SATURATION, self.saturation / 100.0)
```

### خصائص الكاميرا:

| الخاصية | القيمة الافتراضية | المجال |
|---------|------------------|--------|
| Resolution | 640×480 | حسب الكاميرا |
| FPS | 24 | 1-30 |
| JPEG Quality | 70 | 0-100 |
| Brightness | 50 | 0-100 |
| Contrast | 50 | 0-100 |
| Saturation | 50 | 0-100 |

---

## 3. خط أنابيب المرشحات (Filter Pipeline)

### مخطط التدفق:

```
Input Frame (BGR)
     │
     ▼
[1. Color Conversion] ──→ RGB / Grayscale / Binary / BGR
     │
     ▼
[2. Edge Detection]  ──→ Canny Edges (اختياري)
     │
     ▼
[3. Gaussian Blur]   ──→ Blur (اختياري)
     │
     ▼
[4. ROI Crop]        ──→ Region of Interest (اختياري)
     │
     ▼
[5. Resize]          ──→ Scaling (اختياري)
     │
     ▼
Output Frame (BGR, 3 channels)
```

### الخوارزمية الكاملة:

```python
def _apply_filters(self, frame: np.ndarray) -> np.ndarray:
    
    # ── 1. تحويل الألوان ──
    if self.color_mode == "RGB":
        frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
    
    elif self.color_mode == "Grayscale":
        frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)  # رجوع لـ 3 channels
    
    elif self.color_mode == "Binary":
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        _, frame = cv2.threshold(gray, self.threshold_value, 255, cv2.THRESH_BINARY)
        frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
    
    # BGR = لا تحويل (افتراضي OpenCV)
    
    # ── 2. كشف الحواف (Edge Detection) ──
    if self.edge_detection:
        edges = cv2.Canny(frame, 100, 200)
        frame = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)
    
    # ── 3. Gaussian Blur ──
    if self.blur:
        k = self.blur_kernel
        if k % 2 == 0: k += 1  # يجب أن يكون فردياً
        frame = cv2.GaussianBlur(frame, (k, k), 0)
    
    # ── 4. ROI (Region of Interest) ──
    if self.roi_enabled and self.roi_rect:
        x, y, w, h = self.roi_rect
        h_img, w_img = frame.shape[:2]
        x = max(0, min(x, w_img - 1))  # bounds check
        y = max(0, min(y, h_img - 1))
        w = min(w, w_img - x)
        h = min(h, h_img - y)
        if w > 0 and h > 0:
            frame = frame[y:y+h, x:x+w]  # NumPy slicing
    
    # ── 5. تحجيم ──
    if self.resize_factor != 1.0 and self.resize_factor > 0:
        frame = cv2.resize(frame, None,
                          fx=self.resize_factor,
                          fy=self.resize_factor,
                          interpolation=cv2.INTER_LINEAR)
    
    return frame
```

---

## 4. مرشح Binary Threshold — الشرح المفصّل

```python
gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
_, frame = cv2.threshold(gray, self.threshold_value, 255, cv2.THRESH_BINARY)
```

**كيف يعمل Threshold:**
```
لكل بكسل في الصورة الرمادية:
  لو القيمة > threshold_value → بكسل أبيض (255)
  لو القيمة ≤ threshold_value → بكسل أسود (0)

مثال (threshold = 128):
البكسل 200 → 255 (أبيض)
البكسل 50  → 0   (أسود)
البكسل 128 → 0   (أسود)
```

**التطبيقات:**
- كشف الخطوط البيضاء على أرضية داكنة
- فصل الأجسام عن الخلفية
- تحليل الأشكال البسيطة

---

## 5. مرشح Canny Edge Detection — الشرح المفصّل

```python
edges = cv2.Canny(frame, 100, 200)
```

**خوارزمية Canny (5 خطوات):**

```
1. Gaussian Blur: تقليل الضوضاء
2. Sobel Filter: حساب التدرج (Gradient) الأفقي والعمودي
3. Non-Maximum Suppression: تحديد الحواف الرقيقة
4. Double Threshold:
   - فوق 200: حافة قوية (مؤكدة)
   - بين 100-200: حافة ضعيفة (تحتاج اتصال بقوية)
   - تحت 100: ليست حافة (تحذف)
5. Edge Tracking by Hysteresis: ربط الحواف الضعيفة بالقوية
```

---

## 6. مرشح Gaussian Blur — الشرح المفصّل

```python
k = self.blur_kernel
if k % 2 == 0: k += 1
frame = cv2.GaussianBlur(frame, (k, k), 0)
```

**لماذا حجم فردي (odd) فقط؟**
- Gaussian kernel يحتاج مركز محدد
- kernel 5×5 = مركز عند (2,2)
- kernel 4×4 = لا يوجد مركز

**تأثير أحجام مختلفة:**

| Kernel | التأثير |
|--------|---------|
| 3×3 | blur خفيف |
| 5×5 | blur متوسط |
| 7×7 | blur قوي |
| 15×15 | blur شديد جداً |

---

## 7. ROI — Region of Interest

```python
frame = frame[y:y+h, x:x+w]
```

**هذا NumPy array slicing:**
```
الصورة الأصلية: 640×480
ROI: x=100, y=50, w=400, h=300

النتيجة: صورة 400×300 (المنطقة المطلوبة فقط)

الفائدة:
- تقليل حجم البيانات المُرسلة
- التركيز على منطقة مهمة (مثل الطريق أمام الروبوت)
```

---

## 8. خوارزمية البث عبر WebSocket

```python
def _stream_loop(self):
    while self.is_running:
        ret, frame = self.cap.read()
        if not ret:
            time.sleep(0.1)
            continue
        
        # تسجيل لو مفعل
        if self._recording and self._video_writer:
            self._video_writer.write(frame)
        
        # تطبيق المرشحات
        frame = self._apply_filters(frame)
        
        # تحويل لـ JPEG
        encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality]
        success, buffer = cv2.imencode(".jpg", frame, encode_params)
        
        if success and self._socketio:
            # تحويل لـ Base64 string
            encoded = base64.b64encode(buffer.tobytes()).decode("utf-8")
            self._socketio.emit("camera_frame", {"data": encoded})
        
        # التحكم بـ FPS
        time.sleep(1.0 / self.fps)
```

### حسابات الأداء:

```
إطار 640×480 RGB = 640 × 480 × 3 = 921,600 bytes ≈ 900 KB
JPEG quality=70: ≈ 30-50 KB (ضغط ~95%)
Base64: × 1.33 ≈ 40-67 KB لكل إطار

عند 24 FPS:
عرض النطاق المطلوب ≈ 40KB × 24 = 960 KB/s ≈ 1 MB/s
```

---

## 9. التسجيل (Video Recording)

```python
def start_recording(self, filename=None) -> bool:
    fourcc = cv2.VideoWriter_fourcc(*"XVID")  # codec
    self._video_writer = cv2.VideoWriter(
        filepath, fourcc, self.fps,
        (self.frame_width, self.frame_height)
    )
    self._recording = True
```

**Codec: XVID**
- ضغط MPEG-4
- متوافق مع أغلب مشغلات الفيديو
- امتداد: `.avi`

---

## 10. أسئلة للمراجعة

1. لماذا نحوّل Grayscale/Binary مرة أخرى لـ BGR بعد التحويل؟
2. احسب حجم إطار واحد قبل وبعد Base64.
3. ما الفرق بين `cv2.INTER_LINEAR` و `cv2.INTER_NEAREST`؟
4. لماذا `time.sleep(1.0 / self.fps)` وليس `time.sleep(1/24)` مباشرة؟
5. كيف يؤثر JPEG quality على حجم البيانات المُرسلة عبر WebSocket؟
6. لو ROI أكبر من حجم الصورة، ماذا يحدث؟ (تلميح: راجع bounds check)
