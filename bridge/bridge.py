"""PC bridge for the GIGA dashboard.

Sends the PC's local time, the HWiNFO temperatures and loads (CPU, GPU, RAM) and the song Spotify
plays (title, artist, cover) to the Arduino GIGA over USB serial. Protocol: see PROTOCOL.md in the
project root.

Run:  pythonw bridge.py      (autostart: install_autostart.ps1)
Pause the serial port for uploads by creating the file bridge/.pause (tools/build.ps1 -Upload does it).
"""
from __future__ import annotations

import array
import base64
import ctypes
import json
import logging
import logging.handlers
import math
import os
import struct
import sys
import time
import zlib
from pathlib import Path

import serial
import serial.tools.list_ports

from hwinfo import Sensors
from media import NowPlaying, Track

HERE = Path(__file__).resolve().parent
PAUSE_FILE = HERE / ".pause"
SHOT_FILE = HERE / ".shot"      # debug: create it for a screenshot of the display
CMD_FILE = HERE / ".cmd"        # debug: its content is sent to the GIGA as one raw line (e.g. "MEM")
SHOT_DIR = HERE.parent / "screenshots"
COVER_CHUNK = 144               # raw bytes per A line (Dashboard/config.h)
COVER_LINES_PER_PASS = 8        # paces a cover (128 lines) so time and sensor lines are not held up
COVER_RETRY_S = 3.0             # resend a cover the GIGA has not confirmed by then
COVER_TRIES = 3

log = logging.getLogger("bridge")


def setup_logging() -> None:
    (HERE / "logs").mkdir(exist_ok=True)
    handler = logging.handlers.RotatingFileHandler(HERE / "logs" / "bridge.log", maxBytes=512_000,
                                                   backupCount=2, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s"))
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.addHandler(handler)
    if sys.stdout is not None:  # console run (python.exe); pythonw has no stdout
        sys.stdout.reconfigure(errors="replace")  # song titles beyond the console's code page
        root.addHandler(logging.StreamHandler(sys.stdout))


def single_instance() -> bool:
    ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\GigaDashboardBridge")
    return ctypes.windll.kernel32.GetLastError() != 183  # ERROR_ALREADY_EXISTS


def fmt_duration(s: int) -> str:
    h, rest = divmod(s, 3600)
    return f"{h} h {rest // 60} min" if h else f"{rest // 60} min {rest % 60} s" if rest >= 60 else f"{s} s"


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Bridge:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.sensors = Sensors(cfg["sensors"], cfg.get("loads", {}))
        self.media = NowPlaying(cfg.get("media", {}))
        self.ser: serial.Serial | None = None
        self.rx = b""
        self.write_timeouts = 0
        self.shot_at = 0.0
        self.reset_media_state()

    def reset_media_state(self) -> None:
        """After a (re)connect or a GIGA restart: send the song and its cover again."""
        self.media_line = ""                 # last P line sent
        self.cover_acked = 0                 # cover the GIGA confirmed (COVER line)
        self.cover_id = 0                    # cover being sent
        self.cover_queue: list[str] = []     # its remaining A lines
        self.cover_tries = 0
        self.cover_retry_at = 0.0

    # ------------------------------------------------------------------ serial
    def find_port(self) -> str | None:
        sc = self.cfg["serial"]
        if sc.get("port", "auto") != "auto":
            return sc["port"]
        vid, pid = int(sc["usb_vid"], 16), int(sc["usb_pid"], 16)
        for p in serial.tools.list_ports.comports():
            if p.vid == vid and p.pid == pid:
                return p.device
        names = {p.device for p in serial.tools.list_ports.comports()}
        return sc.get("fallback_port") if sc.get("fallback_port") in names else None

    def ensure_serial(self) -> None:
        if PAUSE_FILE.exists():
            if self.ser:
                log.info("pause file present, releasing serial port")
                self.close_serial()
            return
        if self.ser:
            return
        port = self.find_port()
        if not port:
            return
        try:
            self.ser = serial.Serial(port, self.cfg["serial"].get("baud", 115200), timeout=0, write_timeout=1)
        except serial.SerialException as e:
            log.debug("open %s failed: %s", port, e)
            self.ser = None
            return
        log.info("connected to %s", port)
        self.rx = b""
        self.write_timeouts = 0
        self.reset_media_state()

    def close_serial(self) -> None:
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
        self.ser = None

    def send(self, line: str) -> None:
        if not self.ser:
            return
        try:
            self.ser.write(line.encode("utf-8") + b"\n")
            self.write_timeouts = 0
        except serial.SerialTimeoutException:
            # The GIGA is busy (e.g. streaming a screenshot). Drop the line; reconnect if it persists.
            self.write_timeouts += 1
            if self.write_timeouts >= 5:
                log.warning("GIGA stopped reading; reconnecting")
                self.close_serial()

    # ------------------------------------------------------------------ outgoing
    def send_state(self) -> None:
        """Every second: time, temperatures, loads and the song (repeated, so a lost line heals)."""
        now = time.time()
        self.send(f"T|{int(now + time.localtime(now).tm_gmtoff)}")
        temps, loads = self.sensors.read()
        for env, values in (("GIGA_FAKE_TEMPS", temps), ("GIGA_FAKE_LOADS", loads)):  # debug only, e.g. "48,-,38"
            if os.environ.get(env):
                values.update(zip(("cpu", "gpu", "ram"),
                                  (None if v == "-" else float(v) for v in os.environ[env].split(","))))
        self.send("S|" + "|".join("-" if temps.get(k) is None else f"{temps[k]:.1f}"
                                  for k in ("cpu", "gpu", "ram")))
        self.send("L|" + "|".join("-" if loads.get(k) is None else str(round(loads[k]))
                                  for k in ("cpu", "gpu", "ram")))
        self.media_line = ""  # resend P below

    @staticmethod
    def p_line(track: Track | None) -> str:
        if not track:
            return "P"
        cover = f"{track.cover_id:08x}" if track.cover_id else "-"
        return f"P|{cover}|{track.title}|{track.artist}"

    def send_media(self) -> None:
        """The song right when it changes (and every second, see send_state), then its cover."""
        track = self.media.current()
        line = self.p_line(track)
        if line != self.media_line:
            self.send(line)
            self.media_line = line
        if not track or not track.cover_id or track.cover_id == self.cover_acked:
            self.cover_queue = []
            return
        if track.cover_id != self.cover_id:
            self.cover_id, self.cover_tries, self.cover_retry_at = track.cover_id, 0, 0.0
            self.cover_queue = []
        if not self.cover_queue:
            if time.monotonic() < self.cover_retry_at:
                return
            if self.cover_tries >= COVER_TRIES:
                if self.cover_tries == COVER_TRIES:
                    log.warning("GIGA did not confirm cover %08x; old firmware?", track.cover_id)
                    self.cover_tries += 1
                return
            self.cover_tries += 1
            data = track.cover
            self.cover_queue = [f"A|{track.cover_id:08x}|{i // COVER_CHUNK}|"
                                + base64.b64encode(data[i:i + COVER_CHUNK]).decode("ascii")
                                for i in range(0, len(data), COVER_CHUNK)]
            self.cover_queue.reverse()  # pop() from the end
        for _ in range(COVER_LINES_PER_PASS):
            if not self.cover_queue:
                break
            self.send(self.cover_queue.pop())
        if not self.cover_queue:
            self.cover_retry_at = time.monotonic() + COVER_RETRY_S

    # ------------------------------------------------------------------ debug channel
    def check_debug_files(self) -> None:
        if self.shot_at and time.monotonic() >= self.shot_at:
            self.shot_at = 0.0
            self.send("SHOT")
            self.send("MEM")
        if not self.ser:
            return
        if CMD_FILE.exists():
            line = CMD_FILE.read_text(encoding="utf-8", errors="ignore").strip()
            CMD_FILE.unlink(missing_ok=True)
            if line:
                log.info("debug command to GIGA: %s", line)
                self.send(line)
        if SHOT_FILE.exists():
            SHOT_FILE.unlink(missing_ok=True)
            self.shot_at = time.monotonic() + 0.1

    def receive_screenshot(self, w: int, h: int, rot: int) -> None:
        """Framebuffer is physical portrait (w x h, RGB565); the UI is rotated by `rot` (90 or 270)."""
        need = w * h * 2
        data = bytearray(self.rx[:need])
        self.rx = self.rx[need:]
        started = time.monotonic()
        deadline = started + 15
        try:
            self.ser.timeout = 0.05  # block briefly per read instead of spinning
            while len(data) < need and time.monotonic() < deadline:
                data += self.ser.read(min(65536, need - len(data)))
        finally:
            if self.ser:
                self.ser.timeout = 0
        if len(data) < need:
            log.warning("screenshot incomplete (%d of %d bytes)", len(data), need)
            return
        px = array.array("H")
        px.frombytes(bytes(data))
        W, H = h, w  # landscape size
        out = bytearray(W * H * 3)
        i = 0
        for y in range(H):
            for x in range(W):
                # 270: logical (x, y) sits at physical (w-1-y, x); 90: at physical (y, h-1-x)
                v = px[x * w + (w - 1 - y)] if rot == 270 else px[(h - 1 - x) * w + y]
                out[i] = (v >> 11) * 255 // 31
                out[i + 1] = ((v >> 5) & 0x3F) * 255 // 63
                out[i + 2] = (v & 0x1F) * 255 // 31
                i += 3
        SHOT_DIR.mkdir(exist_ok=True)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        path = SHOT_DIR / f"shot-{stamp}.png"
        write_png(path, W, H, bytes(out))
        # The panel as it is physically mounted (portrait), to check the orientation itself.
        panel = bytearray(w * h * 3)
        for i, v in enumerate(px):
            panel[3 * i] = (v >> 11) * 255 // 31
            panel[3 * i + 1] = ((v >> 5) & 0x3F) * 255 // 63
            panel[3 * i + 2] = (v & 0x1F) * 255 // 31
        write_png(SHOT_DIR / f"shot-{stamp}-panel.png", w, h, bytes(panel))
        log.info("screenshot saved: %s (rotation %d, %.1f s)", path, rot, time.monotonic() - started)

    # ------------------------------------------------------------------ incoming
    def handle(self, line: str) -> None:
        f = line.split("|")
        cmd = f[0]
        try:
            if cmd == "SHOT":
                self.receive_screenshot(int(f[1]), int(f[2]), int(f[3]) if len(f) > 3 else 270)
            elif cmd == "HELLO":
                reset = f[2] if len(f) > 2 else ""
                if reset:
                    log.warning("GIGA restarted after a problem: %s (fw %s)", reset, f[1])
                else:
                    log.info("GIGA says hello (fw %s)", f[1] if len(f) > 1 else "?")
                self.reset_media_state()
                self.send_state()
            elif cmd == "SYNC":
                offline = int(f[2])
                if f[1] == "-":
                    log.info("GIGA clock set from the PC (its RTC had no valid time)")
                else:
                    drift = int(f[1])  # 1 s resolution, so -1..+1 is as good as exact
                    (log.warning if abs(drift) > 2 else log.info)(
                        "GIGA clock resynced after %s without PC; its RTC was off by %+d s",
                        fmt_duration(offline), drift)
            elif cmd == "COVER":
                self.cover_acked = int(f[1], 16)
                log.debug("GIGA has cover %08x", self.cover_acked)
            elif cmd == "MEM":
                total, free, biggest, max_used, pct, frag, heap = (int(x) for x in f[1:8])
                log.info("GIGA LVGL heap: %d/%d KB used (%d%%), peak %d KB, biggest free %d KB, frag %d%%; "
                         "largest malloc block %d KB", (total - free) // 1024, total // 1024, pct,
                         max_used // 1024, biggest // 1024, frag, heap // 1024)
            else:
                log.debug("ignored: %s", line)
        except (IndexError, ValueError) as e:
            log.warning("bad line %r: %s", line, e)

    def poll_serial(self) -> None:
        if not self.ser:
            return
        data = self.ser.read(self.ser.in_waiting or 1)
        if not data:
            return
        self.rx += data
        while b"\n" in self.rx:
            raw, self.rx = self.rx.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                self.handle(line)
        if len(self.rx) > 4096:
            self.rx = b""

    # ------------------------------------------------------------------ main loop
    def run(self) -> None:
        while True:
            try:
                self.loop()
            except (serial.SerialException, OSError) as e:
                log.warning("serial port lost: %s", e)
            except Exception:  # never die headless; log and carry on
                log.exception("unexpected error in main loop")
            self.close_serial()
            time.sleep(2.0)

    def loop(self) -> None:
        next_tick = math.floor(time.time()) + 1
        next_try = 0.0
        while True:
            if not self.ser and time.monotonic() >= next_try:
                self.ensure_serial()
                next_try = time.monotonic() + 2.0
            elif self.ser and PAUSE_FILE.exists():
                self.ensure_serial()
            now = time.time()
            if now >= next_tick:  # right after each full second, so the GIGA ticks with the PC
                next_tick = math.floor(now) + 1
                self.send_state()
            if self.ser:
                self.send_media()
            self.poll_serial()
            self.check_debug_files()
            time.sleep(max(0.005, min(0.02, next_tick - time.time())))


def main() -> int:
    setup_logging()
    if not single_instance():
        log.info("another bridge is already running; exiting")
        return 0
    cfg = json.loads((HERE / "config.json").read_text(encoding="utf-8"))
    log.info("bridge starting")
    try:
        bridge = Bridge(cfg)
        bridge.media.start()
        bridge.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
