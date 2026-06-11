"""
MotorController module for Mars Rover (relay-based).

The Arduino uses 4-channel relays (ON/OFF only) for DC motors, so there is
NO speed control.  The firmware accepts these JSON commands:

  Movement : {"dir": "F"}  {"dir": "B"}  {"dir": "L"}  {"dir": "R"}  {"dir": "S"}
  Servos   : {"s1": <0-180>}  {"s2": <0-180>}
  E-Stop   : {"stop": true}

This module translates high-level SocketIO events into the above protocol
and sends them to the Arduino via BluetoothManager.send_json().
"""

import logging
from typing import Any, Dict, Optional

logger = logging.getLogger(__name__)


class MotorController:
    """Controls relay-driven DC motors and two hobby servos on the rover."""

    # ── Constants ────────────────────────────────────────────────────────
    SERVO_MIN = 0
    SERVO_MAX = 180
    SERVO_CENTER = 90

    # Valid direction commands the Arduino firmware accepts
    VALID_DIRECTIONS = {"F", "B", "L", "R", "S"}

    # Human-readable preset names for UI labels / future extensions.
    # All resolve to one of the 5 valid relay directions.
    DIRECTION_PRESETS: Dict[str, str] = {
        "F":      "F",
        "B":      "B",
        "L":      "L",
        "R":      "R",
        "S":      "S",
        "FL":     "L",   # front-left  → relay LEFT
        "FR":     "R",   # front-right → relay RIGHT
        "BL":    "L",    # back-left   → relay LEFT
        "BR":    "R",    # back-right  → relay RIGHT
        "SPIN_L": "L",   # spin left   → relay LEFT
        "SPIN_R": "R",   # spin right  → relay RIGHT
    }

    def __init__(self, bt_manager: Any, socketio: Any = None) -> None:
        """
        Initialise the MotorController.

        Args:
            bt_manager: A BluetoothManager instance with a ``send_json(data)`` method.
            socketio:   Optional SocketIO instance for emitting ``motor_status`` events.
        """
        self._bt_manager = bt_manager
        self._socketio = socketio

        # Current state
        self._direction: str = "S"  # last direction sent (stopped by default)
        self._s1_angle: int = self.SERVO_CENTER
        self._s2_angle: int = self.SERVO_CENTER

        # Compatibility attributes set by app.py config loader (not used by
        # relay logic, but kept so app.py doesn't break on assignment).
        self.default_speed: int = 200
        self.max_speed: int = 255

        logger.info(
            "MotorController initialised – relay mode (bt_manager=%s)",
            type(bt_manager).__name__,
        )

    # ── Public helpers ──────────────────────────────────────────────────

    def emergency_stop(self) -> Dict[str, Any]:
        """Immediately stop all motors and send emergency stop to Arduino.

        Sends both ``{"dir": "S"}`` (direction stop) and ``{"stop": true}``
        (firmware-level emergency stop) to be absolutely safe.
        """
        logger.warning("EMERGENCY STOP triggered")

        self._direction = "S"
        self._send({"dir": "S"})
        self._send({"stop": True})
        self._emit_status("emergency_stop")
        return {"status": "ok", "action": "emergency_stop"}

    def center_servos(self) -> Dict[str, Any]:
        """Move both servos to the centre position (90°)."""
        logger.info("Centering servos")
        return self.set_servo_angle(s1=self.SERVO_CENTER, s2=self.SERVO_CENTER)

    # ── DC motor control (relay-based, no speed) ────────────────────────

    def set_motor_speed(
        self,
        m1: Optional[int] = None,
        m2: Optional[int] = None,
        ramp: bool = False,
    ) -> Dict[str, Any]:
        """Legacy interface kept for app.py compatibility.

        Since relays don't support speed control, this method interprets the
        intent: non-zero values → forward, zero → stop.

        Args:
            m1:   Ignored for speed; if 0 together with m2 → stop.
            m2:   Ignored for speed; if 0 together with m1 → stop.
            ramp: Ignored (relays are instant ON/OFF).
        """
        # Determine direction from sign: both zero → stop, else forward
        val_m1 = m1 if m1 is not None else 0
        val_m2 = m2 if m2 is not None else 0

        if val_m1 == 0 and val_m2 == 0:
            direction = "S"
        elif val_m1 < 0 and val_m2 < 0:
            direction = "B"
        elif val_m1 < 0 or val_m2 > val_m1:
            direction = "R"
        elif val_m2 < 0 or val_m1 > val_m2:
            direction = "L"
        else:
            direction = "F"

        self._direction = direction
        self._send({"dir": direction})
        self._emit_status("set_motor_speed")
        logger.info("set_motor_speed → direction=%s (relay mode)", direction)
        return {"status": "ok", "action": "set_motor_speed", "direction": direction}

    # ── Servo control ───────────────────────────────────────────────────

    def set_servo_angle(
        self,
        s1: Optional[int] = None,
        s2: Optional[int] = None,
    ) -> Dict[str, Any]:
        """Set servo angles (0–180°).

        Sends individual ``{"s1": angle}`` / ``{"s2": angle}`` commands
        matching the Arduino serial protocol.

        Args:
            s1: Angle for servo 1. ``None`` leaves unchanged.
            s2: Angle for servo 2. ``None`` leaves unchanged.
        """
        result: Dict[str, Any] = {"status": "ok", "action": "set_servo_angle"}

        if s1 is not None:
            angle = self._clamp_servo(int(s1))
            self._s1_angle = angle
            self._send({"s1": angle})
            result["s1"] = angle

        if s2 is not None:
            angle = self._clamp_servo(int(s2))
            self._s2_angle = angle
            self._send({"s2": angle})
            result["s2"] = angle

        self._emit_status("set_servo_angle")
        logger.info("Servos set → s1=%d°, s2=%d°", self._s1_angle, self._s2_angle)
        return result

    # ── High-level move command ─────────────────────────────────────────

    def execute_move(self, data: Dict[str, Any]) -> Dict[str, Any]:
        """Process a move command from the UI.

        Accepted formats:

        1. **Direction command** – ``{"direction": "F"}`` (or any key in
           ``DIRECTION_PRESETS``).  The value is mapped to one of the 5
           relay directions (F/B/L/R/S) and sent as ``{"dir": "X"}``.

        2. **Servo command** – ``{"s1": 90}`` and/or ``{"s2": 45}``.
           Sent as individual ``{"s1": angle}`` / ``{"s2": angle}`` payloads.

        Returns a status dict describing what was executed.
        """
        if not isinstance(data, dict):
            logger.error("execute_move received non-dict data: %s", data)
            return {"status": "error", "message": "data must be a dict"}

        direction = data.get("direction")
        result: Dict[str, Any] = {"status": "ok"}

        # ── Direction command ────────────────────────────────────────────
        if direction is not None:
            direction = str(direction).upper()

            # Resolve preset aliases (FL, SPIN_L, …) to valid relay direction
            resolved = self.DIRECTION_PRESETS.get(direction)
            if resolved is None:
                logger.warning("Unknown direction command: '%s'", direction)
                return {"status": "error", "message": f"unknown direction: {direction}"}

            self._direction = resolved
            self._send({"dir": resolved})
            self._emit_status(f"direction:{direction}")

            logger.info("Direction '%s' → relay '%s'", direction, resolved)
            result.update({
                "action": "direction",
                "direction": resolved,
                "preset": direction,
            })
            return result

        # ── Servo-only or mixed command ──────────────────────────────────
        s1 = data.get("s1")
        s2 = data.get("s2")

        if s1 is not None or s2 is not None:
            servo_result = self.set_servo_angle(
                s1=int(s1) if s1 is not None else None,
                s2=int(s2) if s2 is not None else None,
            )
            result.update(servo_result)

        # If no recognized keys were found, log a warning
        if direction is None and s1 is None and s2 is None:
            logger.warning("execute_move: no actionable keys in data: %s", data)
            result.update({"action": "noop", "message": "no actionable keys"})

        return result

    # ── State query ─────────────────────────────────────────────────────

    def get_state(self) -> Dict[str, Any]:
        """Return the current motor and servo state.

        Returns direction ("F"/"B"/"L"/"R"/"S") instead of individual motor
        speeds since relays don't support speed control.
        """
        return {
            "direction": self._direction,
            "s1": self._s1_angle,
            "s2": self._s2_angle,
        }

    # ── Internal helpers ────────────────────────────────────────────────

    def _send(self, payload: Dict[str, Any]) -> None:
        """Send a JSON payload via the Bluetooth manager."""
        try:
            self._bt_manager.send_json(payload)
            logger.debug("Sent BT payload: %s", payload)
        except Exception:
            logger.exception("Failed to send BT payload: %s", payload)

    def _emit_status(self, source: str) -> None:
        """Emit a ``motor_status`` SocketIO event with the current state."""
        if self._socketio is None:
            return
        try:
            state = self.get_state()
            state["source"] = source
            self._socketio.emit("motor_status", state)
            logger.debug("Emitted motor_status (source=%s)", source)
        except Exception:
            logger.exception("Failed to emit motor_status event")

    @classmethod
    def _clamp_servo(cls, value: int) -> int:
        """Clamp a servo angle to [0, 180]."""
        return max(cls.SERVO_MIN, min(cls.SERVO_MAX, int(value)))
