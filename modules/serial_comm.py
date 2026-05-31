"""
serial_comm.py — وحدة الاتصال السيريال عبر PySerial
تغلف PySerial مع قفل خيوط لمنع التعارض
مُحسَّن للتعامل مع JDY-31-SPP (بيانات مكسورة + أول بايتات قمامة)
"""

import serial
import threading
import logging
import json

logger = logging.getLogger(__name__)


class SerialCommunicator:
    """يغلف PySerial مع قفل خيوط لمنع تعارض القراءة/الكتابة"""

    def __init__(self, port: str, baudrate: int = 9600):
        self.port = port
        self.baudrate = baudrate
        self.connection = None
        self.lock = threading.Lock()
        self._line_buffer = b""  # buffer for partial lines

    def open(self) -> bool:
        """يفتح الاتصال السيريال"""
        try:
            self.connection = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=1,
                write_timeout=1,
            )
            # انتظار قصير لاستقرار الاتصال
            if self.connection.isOpen():
                # امسح أي بيانات قديمة بالـ buffer (أول اتصال غالباً قمامة)
                self.connection.reset_input_buffer()
                self.connection.reset_output_buffer()
                self._line_buffer = b""
            logger.info(f"Serial opened: {self.port} @ {self.baudrate}")
            return True
        except serial.SerialException as e:
            logger.error(f"Failed to open serial {self.port}: {e}")
            return False

    def close(self) -> bool:
        """يغلق الاتصال"""
        try:
            if self.connection and self.connection.is_open:
                self.connection.close()
            self._line_buffer = b""
            logger.info(f"Serial closed: {self.port}")
            return True
        except Exception as e:
            logger.error(f"Error closing serial: {e}")
            return False

    def write(self, data: bytes) -> int:
        """يكتب بيانات خام"""
        with self.lock:
            try:
                if self.connection and self.connection.is_open:
                    return self.connection.write(data)
            except Exception as e:
                logger.error(f"Serial write error: {e}")
        return 0

    def write_line(self, line: str) -> bool:
        """يكتب سطر نصي + newline"""
        with self.lock:
            try:
                if self.connection and self.connection.is_open:
                    self.connection.write((line + "\n").encode("utf-8"))
                    self.connection.flush()
                    return True
            except Exception as e:
                logger.error(f"Serial write_line error: {e}")
        return False

    def read_line(self, timeout: float = 1.0) -> str:
        """يقرأ سطر واحد — يتراكم في buffer حتى يلقى \\n"""
        with self.lock:
            try:
                if not (self.connection and self.connection.is_open):
                    return ""

                old_timeout = self.connection.timeout
                self.connection.timeout = timeout

                # اقرأ كل المتاح دفعة وحدة
                waiting = self.connection.in_waiting
                if waiting > 0:
                    chunk = self.connection.read(waiting)
                    self._line_buffer += chunk

                # دور على أول سطر كامل (ينتهي بـ \n أو \r\n)
                while b"\n" not in self._line_buffer:
                    byte = self.connection.read(1)
                    if not byte:
                        break  # timeout
                    self._line_buffer += byte

                self.connection.timeout = old_timeout

                # افصل أول سطر كامل
                if b"\n" in self._line_buffer:
                    line_bytes, rest = self._line_buffer.split(b"\n", 1)
                    self._line_buffer = rest
                    line = line_bytes.decode("utf-8", errors="ignore").strip()
                    if line:
                        return line

            except Exception as e:
                logger.debug(f"Serial read error: {e}")
        return ""

    def read_json(self, timeout: float = 1.0) -> dict:
        """يقرأ سطر ويحوله من JSON إلى dict
        يتخطى الأسطر المكسورة تلقائياً"""
        for _ in range(10):  # جرب حتى 10 أسطر مكسورة
            line = self.read_line(timeout)
            if not line:
                return None
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                logger.debug(f"Skipping malformed line: {line[:80]}")
                continue
        return None

    def is_open(self) -> bool:
        """هل الاتصال مفتوح؟"""
        return self.connection is not None and self.connection.is_open

    def flush(self):
        """يمسح المخزن المؤقت"""
        try:
            if self.connection and self.connection.is_open:
                self.connection.reset_input_buffer()
                self.connection.reset_output_buffer()
                self._line_buffer = b""
        except Exception:
            pass
