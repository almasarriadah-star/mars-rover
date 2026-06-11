# 📋 UserReport - Mars Rover V2 Fix

## 🏗️ نظرة عامة
مشروع Mars Rover: روبوت يُتحكَّم فيه عبر Raspberry Pi + Arduino Uno عبر Bluetooth (HC-05).
تم إصلاح مشكلتين رئيسيتين:
1. فقدان أوامر السيريال (الأردوينو لا يستجيب)
2. عدم عمل أزرار التحكم من الراسبيري + عدم عمل الكاميرا

## 📝 سجل التغييرات
| التاريخ | التغيير | الملفات المتأثرة |
|---------|---------|------------------|
| 2025-06-11 | إنشاء Arduino Firmware V2 — RX-priority architecture | `arduino/robot_controller_v2/robot_controller_v2.ino` |
| 2025-06-11 | إصلاح MotorController — بروتوكول Relay direction بدل m1/m2 speed | `modules/motors.py` |
| 2025-06-11 | إعادة كتابة CameraManager — دعم Pi Camera v5 عبر picamera2 | `modules/camera.py` |
| 2025-06-11 | إصلاح Frontend — sensor keys, camera frame key, motor status | `templates/index.html` |
| 2025-06-11 | إضافة picamera2 للمتطلبات | `requirements.txt` |

## 🐛 المشاكل والحلول

### المشكلة 1: الأردوينو لا يستجيب للأوامر (يحتاج 5 محاولات)
| السبب الجذري | الحل |
|-------------|------|
| SoftwareSerial نصف مزدوج — أثناء إرسال حساسات (كل 1s) + heartbeat (كل 2s) لا يستقبل أوامر | **RX-Priority Architecture**: تفريغ buffer الاستقبال أولاً قبل أي إرسال |
| تردد إرسال عالي يسد القناة | زيادة فترة الحساسات إلى 2000ms والـ heartbeat إلى 5000ms |
| Failsafe 800ms يوقف المحركات فوراً | حُذف بالكامل — المحركات تعمل حتى أمر توقف صريح |
| بافر أوامر صغير (128 بايت) | رُفع إلى 256 بايت |

### المشكلة 2: أزرار الراسبيري لا تحرك الروبوت
| السبب الجذري | الحل |
|-------------|------|
| MotorController يرسل `{"m1":200,"m2":200}` لكن الأردوينو يفهم فقط `{"dir":"F"}` | أُعيد كتابة MotorController ليرسل `{"dir":"F/B/L/R/S"}` |
| الريليهات ON/OFF فقط، لا تدعم سرعة — منطق m1-m4 بلا فائدة | حُذف كل منطق السرعة والـ ramping |

### المشكلة 3: كاميرا Pi Camera v5 لا تعمل
| السبب الجذري | الحل |
|-------------|------|
| Pi Camera Module 5 تحتاج libcamera stack، لا تعمل مع `cv2.VideoCapture(0)` | أُعيد كتابة CameraManager لاستخدام `picamera2` مع fallback لـ OpenCV |

### المشكلة 4: بيانات الحساسات لا تظهر في الواجهة
| السبب الجذري | الحل |
|-------------|------|
| الـ Frontend يبحث عن `data.temperature` لكن الأردوينو يرسل `data.T` | تم تصحيح sensor badges لاستخدام keys الأردوينو (T,H,D,G,R) |
| الـ Frontend يبحث عن `data.image` لكن الكاميرا ترسل `data.data` | تم تصحيح camera_frame handler |

## 💻 ملاحظات التثبيت

### على الراسبيري:
```bash
# تثبيت picamera2 (إن لم يكن موجوداً)
sudo apt install -y python3-picamera2

# أو عبر pip
pip install picamera2
```

### على الأردوينو:
1. افتح `arduino/robot_controller_v2/robot_controller_v2.ino` في Arduino IDE
2. تأكد من تثبيت المكتبات: `ArduinoJson`, `DHT`, `Servo`
3. ارفع الكود على Arduino Uno

## ⚠️ ملاحظات مهمة
- تأكد من فصل Motor Shield القديم فيزيائياً (الريليهات تستخدم D4-D7)
- كاميرا Pi Camera v5 يجب أن تكون مفعلة في `raspi-config` → Interface Options → Camera
- HC-05 يجب أن يكون مقترن (paired) قبل الاتصال
