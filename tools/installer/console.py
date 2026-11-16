"""Small reset-aware Chromatic serial console adapter (pyserial loaded lazily)."""
import re
import time


class Console:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.05
        self.ser.dtr = self.ser.rts = False
        self.buf = ""

    def __enter__(self):
        try:
            self.ser.open()
            self.ser.dtr = self.ser.rts = False
            if not self.pump(8, r"mcu>"):
                self.fresh()
            self.pump(3)
            self.boot = self.buf
            return self
        except Exception:
            self.ser.close()
            raise

    def __exit__(self, *args):
        self.ser.close()

    def pump(self, seconds, pattern=None):
        end = time.monotonic() + seconds
        mark = len(self.buf)
        while time.monotonic() < end:
            self.buf += self.ser.read(4096).decode("utf-8", errors="replace")
            if pattern and re.search(pattern, self.buf[mark:]):
                return True
        return False

    def fresh(self):
        for _ in range(8):
            self.ser.write(b"\r\n")
            if self.pump(1.5, r"mcu>"):
                return
        raise RuntimeError("no fresh mcu> prompt")

    def command(self, command, timeout=90):
        self.fresh()
        self.ser.write(b"pwr window 60000\r\n")
        if not self.pump(2, r"mcu>"):
            raise RuntimeError("no prompt after power window")
        mark = len(self.buf)
        self.ser.write(command.encode("utf-8") + b"\r\n")
        if not self.pump(timeout, r"mcu>"):
            raise RuntimeError("console timeout: " + command)
        self.pump(1)
        return self.buf[mark:]
