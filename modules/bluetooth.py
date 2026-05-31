"""
bluetooth.py — إدارة البلوتوث والاتصال مع HC-05
بحث، اتصال، إعادة اتصال تلقائي، إرسال/استقبال بيانات
"""

import subprocess
import threading
import time
import json
import logging

from modules.serial_comm import SerialCommunicator

logger = logging.getLogger(__name__)


class BluetoothManager:
    """إدارة اتصال بلوتوث مع HC-05 عبر RFCOMM"""

    def __init__(self, config: dict):
        cfg = config or {}
        self.target_name = cfg.get("device_name", "HC-05")
        self.target_address = cfg.get("device_address", "")
        self.rfcomm_port = cfg.get("rfcomm_port", 0)
        self.serial_port = cfg.get("serial_port", "/dev/rfcomm0")
        self.baudrate = cfg.get("baudrate", 9600)
        self.auto_reconnect = cfg.get("auto_reconnect", True)
        self.reconnect_attempts = cfg.get("reconnect_attempts", 5)
        self.reconnect_delay = cfg.get("reconnect_delay", 3)
        self.scan_timeout = cfg.get("scan_timeout", 10)

        self.is_connected = False
        self.discovered_devices = []
        self._serial_comm = None
        self._monitor_thread = None
        self._reader_thread = None
        self._running = False
        self._socketio = None       # يُعين لاحقاً من app.py
        self._sensor_manager = None  # يُعين لاحقاً من app.py (F9)
        self._last_hb = 0            # وقت آخر heartbeat من الأردوينو (F9)

    # ─────────────────────────── بحث ───────────────────────────

    def scan_devices(self, timeout: int = None) -> list:
        """يبحث عن أجهزة البلوتوث القريبة باستخدام bluetoothctl"""
        timeout = timeout or self.scan_timeout
        self.discovered_devices = []

        try:
            # استخدام bluetoothctl للبحث
            result = subprocess.run(
                ["bluetoothctl", "--timeout", str(timeout), "scan", "on"],
                capture_output=True, text=True, timeout=timeout + 5,
            )
            output = result.stdout + result.stderr

            # تحليل النتائج
            seen = set()
            for line in output.split("\n"):
                line = line.strip()
                if "Device" in line:
                    parts = line.split("Device", 1)
                    if len(parts) == 2:
                        rest = parts[1].strip()
                        addr_end = rest.find(" ")
                        if addr_end > 0:
                            addr = rest[:addr_end].strip()
                            name = rest[addr_end:].strip()
                            if addr not in seen:
                                seen.add(addr)
                                self.discovered_devices.append({
                                    "name": name or addr,
                                    "address": addr,
                                })

        except subprocess.TimeoutExpired:
            pass
        except FileNotFoundError:
            # fallback: استخدامه hcitool
            try:
                result = subprocess.run(
                    ["hcitool", "scan", "--flush"],
                    capture_output=True, text=True, timeout=timeout + 5,
                )
                for line in result.stdout.strip().split("\n")[1:]:
                    parts = line.strip().split("\t")
                    if len(parts) >= 2:
                        self.discovered_devices.append({
                            "name": parts[-1].strip(),
                            "address": parts[0].strip(),
                        })
            except Exception as e:
                logger.error(f"Scan failed: {e}")

        logger.info(f"Found {len(self.discovered_devices)} devices")
        return self.discovered_devices

    # ─────────────────────────── اتصال ───────────────────────────

    def connect(self, address: str = None) -> bool:
        """يتصل بجهاز HC-05 عبر RFCOMM"""
        address = address or self.target_address
        if not address:
            logger.error("No target address specified")
            return False

        # تحرير أي اتصال سابق
        self._release_rfcomm()

        try:
            # 1. Pairing
            subprocess.run(
                ["bluetoothctl", "pair", address],
                capture_output=True, timeout=15,
            )

            # 2. Trust
            subprocess.run(
                ["bluetoothctl", "trust", address],
                capture_output=True, timeout=5,
            )

            # 3. Bind RFCOMM
            result = subprocess.run(
                ["sudo", "rfcomm", "bind", str(self.rfcomm_port), address, "1"],
                capture_output=True, text=True, timeout=10,
            )
            if result.returncode != 0:
                logger.error(f"rfcomm bind failed: {result.stderr}")
                # محاولة بدون sudo
                result = subprocess.run(
                    ["rfcomm", "bind", str(self.rfcomm_port), address, "1"],
                    capture_output=True, text=True, timeout=10,
                )

            # 4. فتح Serial
            self._serial_comm = SerialCommunicator(self.serial_port, self.baudrate)
            if self._serial_comm.open():
                self.is_connected = True
                self.target_address = address
                logger.info(f"Connected to {address}")
                self._emit_status()
                return True
            else:
                logger.error("Failed to open serial port")
                return False

        except Exception as e:
            logger.error(f"Connection error: {e}")
            self.is_connected = False
            self._emit_status()
            return False

    def disconnect(self) -> bool:
        """يقطع الاتصال"""
        try:
            if self._serial_comm:
                self._serial_comm.close()
                self._serial_comm = None

            self._release_rfcomm()
            self.is_connected = False
            logger.info("Disconnected")
            self._emit_status()
            return True
        except Exception as e:
            logger.error(f"Disconnect error: {e}")
            return False

    def _release_rfcomm(self):
        """يحرر منفذ RFCOMM"""
        try:
            subprocess.run(
                ["rfcomm", "release", str(self.rfcomm_port)],
                capture_output=True, timeout=5,
            )
        except Exception:
            pass

    # ─────────────────────────── auto-connect ───────────────────────────

    def auto_connect(self) -> bool:
        """يحاول الاتصال تلقائياً بآخر جهاز أو HC-05"""
        # لو عندنا عنوان محفوظ
        if self.target_address:
            return self.connect(self.target_address)

        # بحث عن HC-05
        devices = self.scan_devices(timeout=5)
        for device in devices:
            name = device.get("name", "").upper()
            if any(kw in name for kw in ("HC-05", "HC05", "JDY-31", "JDY31", self.target_name.upper())):
                if self.connect(device["address"]):
                    return True

        logger.warning("Auto-connect failed: HC-05 not found")
        return False

    def auto_connect_with_retry(self) -> bool:
        """إعادة محاولة الاتصال عدة مرات"""
        for attempt in range(1, self.reconnect_attempts + 1):
            logger.info(f"Auto-connect attempt {attempt}/{self.reconnect_attempts}")
            if self.auto_connect():
                return True
            time.sleep(self.reconnect_delay)
        return False

    # ─────────────────────────── إرسال ───────────────────────────

    def send_data(self, data: str) -> bool:
        """يرسل نص عبر السيريال"""
        if not self.is_connected or not self._serial_comm:
            return False
        return self._serial_comm.write_line(data)

    def send_json(self, data: dict) -> bool:
        """يحول dict إلى JSON ويرسله"""
        try:
            return self.send_data(json.dumps(data))
        except Exception as e:
            logger.error(f"Send JSON error: {e}")
            return False

    # ─────────────────────────── استقبال ───────────────────────────

    def read_data(self, timeout: float = 0.5) -> str:
        """يقرأ سطر من السيريال"""
        if not self._serial_comm:
            return ""
        return self._serial_comm.read_line(timeout)

    def read_json(self, timeout: float = 0.5) -> dict:
        """يقرأ JSON من السيريال"""
        if not self._serial_comm:
            return None
        return self._serial_comm.read_json(timeout)

    # ─────────────────────────── حالة ───────────────────────────

    def get_status(self) -> dict:
        """يرجع حالة الاتصال الحالية"""
        return {
            "connected": self.is_connected,
            "address": self.target_address,
            "name": self.target_name,
            "port": self.serial_port,
            "baudrate": self.baudrate,
        }

    def _emit_status(self):
        """يرسل حالة الاتصال عبر WebSocket"""
        if self._socketio:
            try:
                self._socketio.emit("bluetooth_status", self.get_status())
            except Exception:
                pass

    # ─────────────────────────── خيوط خلفية ───────────────────────────

    def start_monitor(self, socketio):
        """يشغل خيوط المراقبة والقراءة"""
        self._socketio = socketio
        self._running = True

        # خيط مراقبة الاتصال
        self._monitor_thread = threading.Thread(
            target=self._connection_monitor_loop, daemon=True
        )
        self._monitor_thread.start()

        # خيط قراءة البيانات
        self._reader_thread = threading.Thread(
            target=self._data_reader_loop, daemon=True
        )
        self._reader_thread.start()

        logger.info("Bluetooth monitor threads started")

    def stop_monitor(self):
        """يوقف خيوط الخلفية"""
        self._running = False

    def _connection_monitor_loop(self):
        """يراقب حالة الاتصال كل 5 ثواني"""
        while self._running:
            try:
                if self.is_connected and self._serial_comm:
                    # فحص هل الاتصال لا يزال مفتوحاً
                    if not self._serial_comm.is_open():
                        self.is_connected = False
                        self._emit_status()
                        logger.warning("Bluetooth connection lost")

                # إعادة اتصال تلقائي فقط إذا كانت مفعّلة ومحفوظ فيها عنوان
                if not self.is_connected and self.auto_reconnect and self.target_address:
                    logger.info("Auto-reconnect: attempting...")
                    if self.auto_connect():
                        logger.info("Auto-reconnect: success")

            except Exception as e:
                logger.error(f"Monitor error: {e}")

            time.sleep(5)

    def _data_reader_loop(self):
        """يقرأ البيانات الواردة من الأردوينو ويوجّهها حسب نوعها (F9)"""
        # أكواد الحساسات المعتمدة من البروتوكول الموحّد (04_json_protocol.md)
        SENSOR_CODES = {"T", "H", "G", "D", "R", "L", "P"}

        while self._running:
            try:
                if self.is_connected and self._serial_comm:
                    data = self.read_json(timeout=0.5)
                    if not isinstance(data, dict):
                        continue

                    if "hb" in data:
                        # نبضة حياة من الأردوينو
                        self._last_hb = time.time()

                    elif "err" in data:
                        # إشعار خطأ من الأردوينو
                        logger.warning("Arduino error: %s", data.get("err"))
                        if self._socketio:
                            self._socketio.emit("log", {
                                "level": "error",
                                "message": f"Arduino: {data.get('err')}",
                            })

                    elif "cfg" in data:
                        # ردود معايرة EEPROM (dump/saved/reset_done)
                        if self._socketio:
                            self._socketio.emit("calib_config", data)

                    else:
                        # قراءات حساسات — مرشَّحة بأكواد البروتوكول
                        readings = {k: v for k, v in data.items() if k in SENSOR_CODES}
                        if readings:
                            if self._sensor_manager is not None:
                                try:
                                    self._sensor_manager.update(readings)
                                except Exception as e:
                                    logger.error("sensor_manager.update error: %s", e)
                            if self._socketio:
                                self._socketio.emit("sensor_data", readings)

            except Exception as e:
                logger.debug(f"Reader error: {e}")

            time.sleep(0.1)

    # ─────────────────────────── إعدادات ───────────────────────────

    def update_config(self, config: dict):
        """يحدث إعدادات البلوتوث"""
        if "device_name" in config:
            self.target_name = config["device_name"]
        if "device_address" in config:
            self.target_address = config["device_address"]
        if "baudrate" in config:
            self.baudrate = config["baudrate"]
        if "auto_reconnect" in config:
            self.auto_reconnect = config["auto_reconnect"]
        if "reconnect_attempts" in config:
            self.reconnect_attempts = config["reconnect_attempts"]
        if "reconnect_delay" in config:
            self.reconnect_delay = config["reconnect_delay"]
