"""Temperature and load readings from HWiNFO64.

Primary source: HWiNFO's shared memory (Settings > "Shared Memory Support").
Fallback: the registry values HWiNFO writes for sensors marked "Report value in Gadget"
(HKCU\\Software\\HWiNFO64\\VSB). The fallback has no 12 h limit in the free version.
"""
from __future__ import annotations

import ctypes
import logging
import re
import struct
import winreg
from ctypes import wintypes
from dataclasses import dataclass

log = logging.getLogger("hwinfo")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenFileMappingW.restype = wintypes.HANDLE
_k32.OpenFileMappingW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
_k32.MapViewOfFile.restype = ctypes.c_void_p
_k32.MapViewOfFile.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t)
_k32.UnmapViewOfFile.argtypes = (ctypes.c_void_p,)
_k32.CloseHandle.argtypes = (wintypes.HANDLE,)
FILE_MAP_READ = 0x0004

SM_NAME = "Global\\HWiNFO_SENS_SM2"
SIG_ALIVE = 0x53695748  # "HWiS"
HEADER = struct.Struct("<IIIqIIIIII")
SENSOR = struct.Struct("<II128s128s")
READING = struct.Struct("<III128s128s16sdddd")
T_TEMP, T_FAN, T_USAGE = 1, 3, 7


@dataclass
class Reading:
    kind: int          # 1 temperature, 3 fan, 7 usage, 8 other (e.g. "Physical Memory Load")
    sensor: str        # e.g. "CPU [#0]: Intel Core i5-12600K: DTS"
    label: str         # original (English) label, e.g. "CPU Package"
    unit: str
    value: float


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("cp1252", errors="replace").strip()


def read_shared_memory() -> list[Reading] | None:
    handle = _k32.OpenFileMappingW(FILE_MAP_READ, False, SM_NAME)
    if not handle:
        return None  # HWiNFO not running or shared memory switched off
    try:
        view = _k32.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)
        if not view:
            return None
        try:
            hdr = HEADER.unpack(ctypes.string_at(view, HEADER.size))
            sig, _ver, _rev, _poll, off_s, size_s, n_s, off_r, size_r, n_r = hdr
            if sig != SIG_ALIVE or n_r == 0:
                return None
            buf = ctypes.string_at(view, off_r + size_r * n_r)
        finally:
            _k32.UnmapViewOfFile(view)
    finally:
        _k32.CloseHandle(handle)
    sensors = []
    for i in range(n_s):
        o = off_s + i * size_s
        _sid, _inst, name_orig, _name_user = SENSOR.unpack_from(buf, o)
        sensors.append(_cstr(name_orig))
    out = []
    for i in range(n_r):
        o = off_r + i * size_r
        kind, s_idx, _rid, label_orig, _label_user, unit, value, *_ = READING.unpack_from(buf, o)
        out.append(Reading(kind, sensors[s_idx] if s_idx < len(sensors) else "", _cstr(label_orig),
                           _cstr(unit), value))
    return out


def read_gadget_registry() -> list[Reading] | None:
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\HWiNFO64\VSB")
    except OSError:
        return None
    values: dict[str, str] = {}
    with key:
        i = 0
        while True:
            try:
                name, data, _ = winreg.EnumValue(key, i)
            except OSError:
                break
            values[name] = str(data)
            i += 1
    out = []
    idx = 0
    while f"Label{idx}" in values:
        raw = values.get(f"ValueRaw{idx}", "")
        shown = values.get(f"Value{idx}", "")
        try:
            value = float(raw.replace(",", "."))
        except ValueError:
            idx += 1
            continue
        unit = next((u for u in ("°C", "°F", "RPM", "%") if u in shown), "")
        kind = T_TEMP if unit in ("°C", "°F") else T_FAN if unit == "RPM" else T_USAGE if unit == "%" else 0
        out.append(Reading(kind, values.get(f"Sensor{idx}", ""), values[f"Label{idx}"], unit, value))
        idx += 1
    return out or None


def _celsius(r: Reading) -> float:
    return (r.value - 32.0) * 5.0 / 9.0 if r.unit == "°F" else r.value


Rules = dict[str, list[tuple[re.Pattern, re.Pattern]]]


def _compile(rules: dict) -> Rules:
    return {k: [(re.compile(r.get("sensor", "")), re.compile(r["label"])) for r in v] for k, v in rules.items()}


def _pick(readings: list[Reading], rules: Rules, accept, value) -> dict[str, float | None]:
    """Per key: the highest value of the first rule that matches (RAM temperature: hottest DIMM)."""
    out: dict[str, float | None] = {}
    for key, key_rules in rules.items():
        out[key] = None
        for sensor_rx, label_rx in key_rules:
            hits = [value(r) for r in readings if accept(r) and sensor_rx.search(r.sensor) and label_rx.search(r.label)]
            if hits:
                out[key] = max(hits)
                break
    return out


class Sensors:
    """Resolves CPU/GPU/RAM temperatures and loads using regex rules from config."""

    def __init__(self, temp_rules: dict, load_rules: dict) -> None:
        self.temp_rules = _compile(temp_rules)
        self.load_rules = _compile(load_rules)
        self.source = "none"
        self._warned = False

    def read(self) -> tuple[dict[str, float | None], dict[str, float | None]]:
        """(temperatures in °C, loads in %); None where HWiNFO has no value."""
        readings = read_shared_memory()
        source = "shared memory"
        if readings is None:
            readings = read_gadget_registry()
            source = "gadget registry"
        if readings is None:
            if self.source != "none" or not self._warned:
                log.warning("no HWiNFO data (shared memory off and no gadget values)")
                self._warned = True
            self.source = "none"
            return {k: None for k in self.temp_rules}, {k: None for k in self.load_rules}
        if source != self.source:
            log.info("HWiNFO source: %s (%d readings)", source, len(readings))
            self.source = source
        temps = _pick(readings, self.temp_rules, lambda r: r.kind == T_TEMP, _celsius)
        loads = _pick(readings, self.load_rules, lambda r: r.unit == "%", lambda r: r.value)
        return temps, loads
