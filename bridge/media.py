"""What Spotify is playing, read from Windows' media controls.

Spotify reports title, artist and cover to Windows like every media player (the media overlay
next to the volume flyout uses the same data), so no Spotify account or API key is needed.
The WinRT calls are async and depend on another app, so they run in their own thread; the
bridge thread only picks up the latest result and stays the only one touching the serial port.

Needs: winrt-Windows.Media.Control, winrt-Windows.Storage.Streams, Pillow (requirements.txt).
"""
from __future__ import annotations

import asyncio
import io
import logging
import os
import threading
import time
import unicodedata
import zlib
from array import array
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("media")

COVER_PX = 96            # Dashboard/config.h COVER_PX
MAX_TEXT_BYTES = 120     # per field; Dashboard/link.cpp has room for two of them in one line
POLL_S = 1.0
COVER_SETTLE_S = 6.0     # re-read the cover this long after a track change; apps update it late
BACKDROP = (12, 26, 44)  # C_REGISTER, behind transparent covers

# Characters of the GIGA's font_ui_22. Keep in sync with MEDIA_RANGES in tools/build_fonts.py.
_RANGES = ((0x20, 0x7E), (0xA0, 0xFF), (0x100, 0x17F), (0x2010, 0x2014), (0x2018, 0x201E),
           (0x2020, 0x2022), (0x2026, 0x2026), (0x2030, 0x2030), (0x2039, 0x203A),
           (0x20AC, 0x20AC), (0x2122, 0x2122))


@dataclass(frozen=True)
class Track:
    title: str
    artist: str
    cover_id: int = 0    # CRC32 of `cover`, never 0; 0 = no cover
    cover: bytes = b""   # COVER_PX x COVER_PX RGB565, little-endian (the GIGA's byte order)


def _supported(ch: str) -> bool:
    o = ord(ch)
    return any(a <= o <= b for a, b in _RANGES)


def display_text(text: str) -> str:
    """Keep what the display can show: other accented letters lose their accent, emoji and
    symbols are dropped, other scripts become "?". "|" would split the protocol line."""
    out: list[str] = []
    for ch in text.replace("|", "/"):
        if _supported(ch):
            out.append(ch)
        elif ch.isspace():
            out.append(" ")
        else:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))
            if base and all(_supported(c) for c in base):
                out.append(base)
            elif unicodedata.category(ch)[0] in "LN" and (not out or out[-1] != "?"):
                out.append("?")
    text = " ".join("".join(out).split())
    return text.encode("utf-8")[:MAX_TEXT_BYTES].decode("utf-8", errors="ignore")


def make_cover(raw: bytes) -> tuple[int, bytes]:
    """Image file (JPEG/PNG) -> (id, RGB565 pixels), centre-cropped to a square."""
    from PIL import Image, ImageOps
    im = Image.open(io.BytesIO(raw))
    if im.mode in ("RGBA", "LA", "P"):
        bg = Image.new("RGBA", im.size, BACKDROP + (255,))
        bg.alpha_composite(im.convert("RGBA"))
        im = bg
    im = ImageOps.fit(im.convert("RGB"), (COVER_PX, COVER_PX), Image.Resampling.LANCZOS)
    px = array("H", (((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
                     for r, g, b in im.getdata()))
    data = px.tobytes()
    return (zlib.crc32(data) or 1), data


class NowPlaying:
    """Polls the media session of the configured apps once per second in a background thread."""

    def __init__(self, cfg: dict) -> None:
        self.apps = [a.lower() for a in cfg.get("apps", ["Spotify"])]
        self.hold_s = float(cfg.get("hide_after_s", 3))  # bridges the pause between two tracks
        self._lock = threading.Lock()
        self._track: Track | None = None

    def start(self) -> None:
        # Debug only: "title|artist", "title|artist|image file", or "-" for nothing playing.
        fake = os.environ.get("GIGA_FAKE_MEDIA")
        if fake == "-":
            return
        if fake:
            title, artist, *image = fake.split("|")
            cover = make_cover(Path(image[0]).read_bytes()) if image else (0, b"")
            self._track = Track(display_text(title), display_text(artist), *cover)
            return
        try:
            import winrt.windows.media.control  # noqa: F401
            import PIL  # noqa: F401
        except ImportError as e:
            log.warning("now playing disabled: %s (pip install -r requirements.txt)", e)
            return
        threading.Thread(target=lambda: asyncio.run(self._main()), name="media", daemon=True).start()

    def current(self) -> Track | None:
        with self._lock:
            return self._track

    def _find_session(self, manager):
        for s in manager.get_sessions():
            app = (s.source_app_user_model_id or "").lower()
            if any(a in app for a in self.apps):
                return s
        return None

    @staticmethod
    async def _read_thumbnail(ref) -> bytes:
        from winrt.windows.storage.streams import Buffer, InputStreamOptions
        if ref is None:
            return b""
        stream = await ref.open_read_async()
        try:
            buf = Buffer(stream.size)
            await stream.read_async(buf, buf.capacity, InputStreamOptions.READ_AHEAD)
            return bytes(buf)
        finally:
            stream.close()

    async def _main(self) -> None:
        from winrt.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager,
            GlobalSystemMediaTransportControlsSessionPlaybackStatus as Status)
        manager = None
        track: Track | None = None
        key, key_since = None, 0.0
        raw_crc, cover = None, (0, b"")
        playing_until = 0.0
        last_error = ""
        while True:
            now = time.monotonic()
            try:
                if manager is None:
                    manager = await Manager.request_async()
                session = self._find_session(manager)
                if session and session.get_playback_info().playback_status == Status.PLAYING:
                    props = await session.try_get_media_properties_async()
                    if (props.title, props.artist, props.album_title) != key:
                        key, key_since = (props.title, props.artist, props.album_title), now
                        log.info("now playing: %s - %s", props.artist, props.title)
                    if now - key_since <= COVER_SETTLE_S:
                        raw = await self._read_thumbnail(props.thumbnail)
                        if zlib.crc32(raw) != raw_crc:
                            raw_crc = zlib.crc32(raw)
                            cover = make_cover(raw) if raw else (0, b"")
                    track = Track(display_text(props.title), display_text(props.artist), *cover)
                    playing_until = now + self.hold_s
                elif track and now >= playing_until:
                    track, key = None, None
                    log.info("music stopped")
                last_error = ""
            except Exception as e:  # app closed mid-call, WinRT hiccup, broken image ...
                if str(e) != last_error:
                    last_error = str(e)
                    log.warning("media session: %s", e)
                manager = None
                if track and now >= playing_until:
                    track, key = None, None
            with self._lock:
                self._track = track
            await asyncio.sleep(POLL_S)
