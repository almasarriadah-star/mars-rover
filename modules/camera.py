"""
camera.py — Camera system for Raspberry Pi
Supports Pi Camera v5 (via picamera2/libcamera) with OpenCV fallback.
Live streaming via WebSocket + filters (RGB, BGR, Grayscale, Binary)
"""

import base64
import threading
import time
import os
import logging
import numpy as np

logger = logging.getLogger(__name__)

# Try importing picamera2 first (for Pi Camera v5 on CSI port)
_USE_PICAMERA2 = False
try:
    from picamera2 import Picamera2
    _USE_PICAMERA2 = True
    logger.info("picamera2 library available — will use libcamera stack")
except ImportError:
    logger.info("picamera2 not available — will fall back to OpenCV")

import cv2


class CameraManager:
    """Camera manager supporting Pi Camera v5 (picamera2) + USB cameras (OpenCV)"""

    def __init__(self, config: dict):
        cfg = config or {}
        self.camera_index = cfg.get("camera_index", 0)
        self.frame_width = cfg.get("width", 640)
        self.frame_height = cfg.get("height", 480)
        self.fps = cfg.get("fps", 24)
        self.jpeg_quality = cfg.get("jpeg_quality", 70)

        # Color settings
        self.color_mode = cfg.get("default_color_mode", "RGB")
        self.brightness = cfg.get("brightness", 50)
        self.contrast = cfg.get("contrast", 50)
        self.saturation = cfg.get("saturation", 50)

        # Filters
        self.edge_detection = cfg.get("edge_detection", False)
        self.blur = cfg.get("blur", False)
        self.blur_kernel = cfg.get("blur_kernel", 5)
        self.threshold_value = cfg.get("threshold_value", 128)

        # Scaling & ROI
        self.resize_factor = cfg.get("resize_factor", 1.0)
        self.roi_enabled = cfg.get("roi_enabled", False)
        self.roi_rect = tuple(cfg.get("roi_rect", [0, 0, 640, 480]))

        # Rotation / flip
        self.rotate_180 = cfg.get("rotate_180", False)
        self.flip_h = cfg.get("flip_h", False)
        self.flip_v = cfg.get("flip_v", False)

        # Paths
        self.photo_path = cfg.get("photo_path", "photos")
        self.recording_path = cfg.get("recording_path", "recordings")

        # Force backend choice: "picamera2", "opencv", or "auto"
        self.backend = cfg.get("backend", "auto")

        # State
        self._picam = None          # Picamera2 instance
        self._cv_cap = None         # OpenCV VideoCapture
        self._using_picamera2 = False
        self.is_running = False
        self._socketio = None
        self._stream_thread = None
        self._recording = False
        self._video_writer = None
        self._frame_lock = threading.Lock()
        self._latest_frame = None   # cached latest frame (numpy array, BGR)

    # ─────────────────── Start / Stop ───────────────────

    def start(self) -> bool:
        """Open the camera. Tries picamera2 first, then falls back to OpenCV."""
        if self.is_running:
            return True

        # Determine backend
        use_picam = False
        if self.backend == "picamera2":
            use_picam = _USE_PICAMERA2
        elif self.backend == "opencv":
            use_picam = False
        else:  # auto
            use_picam = _USE_PICAMERA2

        if use_picam:
            if self._start_picamera2():
                return True
            logger.warning("picamera2 failed, falling back to OpenCV")

        return self._start_opencv()

    def _start_picamera2(self) -> bool:
        """Open camera via picamera2 (libcamera stack)."""
        try:
            self._picam = Picamera2(camera_num=self.camera_index)

            # Configure for still+video preview
            preview_config = self._picam.create_preview_configuration(
                main={"size": (self.frame_width, self.frame_height),
                      "format": "RGB888"},
            )
            self._picam.configure(preview_config)

            # Apply controls
            controls = {}
            # Brightness: picamera2 uses -1.0 to 1.0 range
            if self.brightness != 50:
                controls["Brightness"] = (self.brightness - 50) / 50.0
            # Contrast: picamera2 uses 0.0 to 32.0, default 1.0
            if self.contrast != 50:
                controls["Contrast"] = self.contrast / 50.0
            # Saturation: picamera2 uses 0.0 to 32.0, default 1.0
            if self.saturation != 50:
                controls["Saturation"] = self.saturation / 50.0

            if controls:
                self._picam.set_controls(controls)

            self._picam.start()
            # Wait for camera to stabilize
            time.sleep(0.5)

            self._using_picamera2 = True
            self.is_running = True
            logger.info(
                "Pi Camera started via picamera2: %dx%d @ %dfps (camera_num=%d)",
                self.frame_width, self.frame_height, self.fps, self.camera_index
            )
            return True

        except Exception as e:
            logger.error("picamera2 start error: %s", e)
            if self._picam:
                try:
                    self._picam.close()
                except Exception:
                    pass
                self._picam = None
            return False

    def _start_opencv(self) -> bool:
        """Open camera via OpenCV VideoCapture (USB cameras)."""
        try:
            self._cv_cap = cv2.VideoCapture(self.camera_index)
            if not self._cv_cap.isOpened():
                logger.error("Cannot open camera %d via OpenCV", self.camera_index)
                return False

            self._cv_cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.frame_width)
            self._cv_cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.frame_height)
            self._cv_cap.set(cv2.CAP_PROP_FPS, self.fps)
            self._cv_cap.set(cv2.CAP_PROP_BRIGHTNESS, self.brightness / 100.0)
            self._cv_cap.set(cv2.CAP_PROP_CONTRAST, self.contrast / 100.0)
            self._cv_cap.set(cv2.CAP_PROP_SATURATION, self.saturation / 100.0)

            self._using_picamera2 = False
            self.is_running = True
            logger.info(
                "Camera started via OpenCV: %dx%d @ %dfps",
                self.frame_width, self.frame_height, self.fps
            )
            return True

        except Exception as e:
            logger.error("OpenCV camera start error: %s", e)
            return False

    def stop(self) -> bool:
        """Stop the camera and release resources."""
        self.is_running = False

        if self._recording:
            self.stop_recording()

        if self._using_picamera2 and self._picam:
            try:
                self._picam.stop()
                self._picam.close()
            except Exception as e:
                logger.error("picamera2 stop error: %s", e)
            self._picam = None

        if self._cv_cap:
            try:
                self._cv_cap.release()
            except Exception:
                pass
            self._cv_cap = None

        logger.info("Camera stopped")
        return True

    # ─────────────────── Frame Capture ───────────────────

    def _capture_raw_frame(self) -> np.ndarray:
        """Capture a raw frame from the active camera backend. Returns BGR numpy array or None."""
        if self._using_picamera2 and self._picam:
            try:
                # picamera2 returns RGB array
                frame = self._picam.capture_array("main")
                if frame is not None:
                    # Convert RGB to BGR for OpenCV compatibility
                    frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                return frame
            except Exception as e:
                logger.debug("picamera2 capture error: %s", e)
                return None

        elif self._cv_cap and self._cv_cap.isOpened():
            ret, frame = self._cv_cap.read()
            return frame if ret else None

        return None

    def get_frame(self) -> bytes:
        """Capture a frame, apply filters, return JPEG bytes."""
        frame = self._capture_raw_frame()
        if frame is None:
            return b""

        frame = self._apply_filters(frame)

        encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality]
        success, buffer = cv2.imencode(".jpg", frame, encode_params)
        if success:
            return buffer.tobytes()
        return b""

    def _apply_filters(self, frame: np.ndarray) -> np.ndarray:
        """Apply enabled filters to the frame (expects BGR input)."""

        # 0. Rotation / Flip
        if self.rotate_180:
            frame = cv2.rotate(frame, cv2.ROTATE_180)
        if self.flip_h:
            frame = cv2.flip(frame, 1)
        if self.flip_v:
            frame = cv2.flip(frame, 0)

        # 1. Color conversion
        if self.color_mode == "RGB":
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        elif self.color_mode == "Grayscale":
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        elif self.color_mode == "Binary":
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            _, frame = cv2.threshold(
                gray, self.threshold_value, 255, cv2.THRESH_BINARY
            )
            frame = cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
        # BGR = OpenCV default (no conversion)

        # 2. Edge Detection
        if self.edge_detection:
            edges = cv2.Canny(frame, 100, 200)
            frame = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)

        # 3. Blur
        if self.blur:
            k = self.blur_kernel if self.blur_kernel % 2 == 1 else self.blur_kernel + 1
            frame = cv2.GaussianBlur(frame, (k, k), 0)

        # 4. ROI
        if self.roi_enabled and self.roi_rect:
            x, y, w, h = self.roi_rect
            h_img, w_img = frame.shape[:2]
            x = max(0, min(x, w_img - 1))
            y = max(0, min(y, h_img - 1))
            w = min(w, w_img - x)
            h = min(h, h_img - y)
            if w > 0 and h > 0:
                frame = frame[y : y + h, x : x + w]

        # 5. Resize
        if self.resize_factor != 1.0 and self.resize_factor > 0:
            frame = cv2.resize(
                frame,
                None,
                fx=self.resize_factor,
                fy=self.resize_factor,
                interpolation=cv2.INTER_LINEAR,
            )

        return frame

    # ─────────────────── Photos & Recording ───────────────────

    def capture_photo(self, filename: str = None) -> str:
        """Capture a photo and save it to disk."""
        frame = self._capture_raw_frame()
        if frame is None:
            return ""

        if not filename:
            timestamp = time.strftime("%Y%m%d_%H%M%S")
            filename = f"photo_{timestamp}.jpg"

        os.makedirs(self.photo_path, exist_ok=True)
        filepath = os.path.join(self.photo_path, filename)

        frame = self._apply_filters(frame)
        cv2.imwrite(filepath, frame)
        logger.info("Photo saved: %s", filepath)
        return filepath

    def start_recording(self, filename: str = None) -> bool:
        """Start video recording."""
        if not self.is_running:
            return False

        if not filename:
            timestamp = time.strftime("%Y%m%d_%H%M%S")
            filename = f"recording_{timestamp}.avi"

        os.makedirs(self.recording_path, exist_ok=True)
        filepath = os.path.join(self.recording_path, filename)

        fourcc = cv2.VideoWriter_fourcc(*"XVID")
        self._video_writer = cv2.VideoWriter(
            filepath, fourcc, self.fps, (self.frame_width, self.frame_height)
        )
        self._recording = True
        logger.info("Recording started: %s", filepath)
        return True

    def stop_recording(self) -> str:
        """Stop video recording."""
        self._recording = False
        filepath = ""
        if self._video_writer:
            self._video_writer.release()
            self._video_writer = None
        logger.info("Recording stopped")
        return filepath

    # ─────────────────── Settings ───────────────────

    def update_settings(self, settings: dict):
        """Update camera settings dynamically."""
        if "color_mode" in settings:
            self.color_mode = settings["color_mode"]
        if "brightness" in settings:
            self.brightness = settings["brightness"]
            self._apply_camera_controls()
        if "contrast" in settings:
            self.contrast = settings["contrast"]
            self._apply_camera_controls()
        if "saturation" in settings:
            self.saturation = settings["saturation"]
            self._apply_camera_controls()
        if "jpeg_quality" in settings:
            self.jpeg_quality = settings["jpeg_quality"]
        if "edge_detection" in settings:
            self.edge_detection = settings["edge_detection"]
        if "blur" in settings:
            self.blur = settings["blur"]
        if "blur_kernel" in settings:
            self.blur_kernel = settings["blur_kernel"]
        if "threshold_value" in settings:
            self.threshold_value = settings["threshold_value"]
        if "resize_factor" in settings:
            self.resize_factor = settings["resize_factor"]
        if "roi_enabled" in settings:
            self.roi_enabled = settings["roi_enabled"]
        if "roi_rect" in settings:
            self.roi_rect = tuple(settings["roi_rect"])
        if "fps" in settings:
            self.fps = settings["fps"]
        if "rotate_180" in settings:
            self.rotate_180 = settings["rotate_180"]
        if "flip_h" in settings:
            self.flip_h = settings["flip_h"]
        if "flip_v" in settings:
            self.flip_v = settings["flip_v"]
        if "width" in settings and "height" in settings:
            self.frame_width = settings["width"]
            self.frame_height = settings["height"]
            # For OpenCV, update capture properties
            if self._cv_cap:
                self._cv_cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.frame_width)
                self._cv_cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.frame_height)

    def _apply_camera_controls(self):
        """Apply brightness/contrast/saturation to the active backend."""
        if self._using_picamera2 and self._picam:
            try:
                controls = {
                    "Brightness": (self.brightness - 50) / 50.0,
                    "Contrast": self.contrast / 50.0,
                    "Saturation": self.saturation / 50.0,
                }
                self._picam.set_controls(controls)
            except Exception as e:
                logger.debug("picamera2 controls error: %s", e)
        elif self._cv_cap:
            self._cv_cap.set(cv2.CAP_PROP_BRIGHTNESS, self.brightness / 100.0)
            self._cv_cap.set(cv2.CAP_PROP_CONTRAST, self.contrast / 100.0)
            self._cv_cap.set(cv2.CAP_PROP_SATURATION, self.saturation / 100.0)

    def get_settings(self) -> dict:
        """Return all current settings."""
        return {
            "camera_index": self.camera_index,
            "width": self.frame_width,
            "height": self.frame_height,
            "fps": self.fps,
            "jpeg_quality": self.jpeg_quality,
            "color_mode": self.color_mode,
            "brightness": self.brightness,
            "contrast": self.contrast,
            "saturation": self.saturation,
            "edge_detection": self.edge_detection,
            "blur": self.blur,
            "blur_kernel": self.blur_kernel,
            "threshold_value": self.threshold_value,
            "resize_factor": self.resize_factor,
            "roi_enabled": self.roi_enabled,
            "roi_rect": list(self.roi_rect),
            "rotate_180": self.rotate_180,
            "flip_h": self.flip_h,
            "flip_v": self.flip_v,
            "backend": "picamera2" if self._using_picamera2 else "opencv",
        }

    # ─────────────────── WebSocket Streaming ───────────────────

    def start_stream(self, socketio):
        """Start the camera streaming thread."""
        self._socketio = socketio
        self._stream_thread = threading.Thread(
            target=self._stream_loop, daemon=True
        )
        self._stream_thread.start()
        logger.info("Camera stream started")

    def stop_stream(self):
        """Stop streaming."""
        self.is_running = False

    def _stream_loop(self):
        """Stream camera frames via WebSocket."""
        frame_interval = 1.0 / max(1, self.fps)

        while self.is_running:
            try:
                frame = self._capture_raw_frame()
                if frame is None:
                    time.sleep(0.1)
                    continue

                # Record raw frame if enabled
                if self._recording and self._video_writer:
                    self._video_writer.write(frame)

                # Apply filters for streaming
                frame = self._apply_filters(frame)

                # Encode to JPEG then base64
                encode_params = [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_quality]
                success, buffer = cv2.imencode(".jpg", frame, encode_params)
                if success and self._socketio:
                    encoded = base64.b64encode(buffer.tobytes()).decode("utf-8")
                    self._socketio.emit("camera_frame", {"data": encoded})

                # FPS throttle
                time.sleep(frame_interval)

            except Exception as e:
                logger.error("Stream error: %s", e)
                time.sleep(0.5)

    # ─────────────────── Info ───────────────────

    def get_available_cameras(self) -> list:
        """Return a list of available camera indices."""
        cameras = []

        # Check picamera2 cameras
        if _USE_PICAMERA2:
            try:
                info = Picamera2.global_camera_info()
                for cam in info:
                    cameras.append({
                        "index": cam.get("Num", 0),
                        "model": cam.get("Model", "unknown"),
                        "backend": "picamera2",
                    })
            except Exception:
                pass

        # Check OpenCV cameras
        for i in range(5):
            cap = cv2.VideoCapture(i)
            if cap.isOpened():
                cameras.append({
                    "index": i,
                    "model": "USB/V4L2",
                    "backend": "opencv",
                })
                cap.release()

        return cameras
