#!/usr/bin/env python3
"""Cairn Freematics ONE+ Model B comprehensive device emulator.

Simulates the full device lifecycle: state machine, GNSS, IMU, battery,
storage, and network sync.  Produces trip bundles in the exact binary
format consumed by the ingest server and exercises every edge case the
firmware must handle.

Python 3.9+, stdlib only (curses optional for TUI).

Usage examples:
    python3 freematics_emulator.py --scenario normal_commute --server http://localhost:8443
    python3 freematics_emulator.py --all-scenarios --server http://localhost:8443
    python3 freematics_emulator.py --scenario normal_commute --devices 3
    python3 freematics_emulator.py --scenario normal_commute --headless --speedup 10
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import signal
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone, timedelta
from enum import Enum, auto
from pathlib import Path
from typing import (
    Any, Callable, Dict, List, NamedTuple, Optional, Sequence, Tuple,
)

# ============================================================================
# Constants -- mirrored from firmware config.h
# ============================================================================

ARMING_DURATION_MS = 15_000         # 15 s
STOP_DWELL_MS = 300_000             # 5 min
MIN_SPEED_KMH = 5
GNSS_RATE_ACTIVE_HZ = 5
GNSS_RATE_SLOW_HZ = 2
GNSS_RATE_STATIONARY_HZ = 1
IMU_RATE_ACTIVE_HZ = 50
IMU_RATE_SLOW_HZ = 25
IMU_RATE_STATIONARY_HZ = 10
LOW_BATTERY_THRESHOLD_MV = 11_500   # 11.5 V
UPLOAD_CHUNK_SIZE = 4096
RETENTION_WINDOW_HOURS = 168        # 7 days
WIFI_SCAN_INTERVAL_MS = 60_000      # 1 min
MIN_FREE_SD_MB = 50
MAX_TRIP_DURATION_MS = 14_400_000   # 4 hours
MAX_HDOP_TENTHS = 50
MIN_SATELLITES = 4
ULP_MOTION_THRESHOLD_MG = 300       # 0.3 g

# Binary struct formats from trip_types.h
GNSS_STRUCT = struct.Struct("<QiiiHHBBHH2s")   # 32 bytes
IMU_STRUCT = struct.Struct("<QHhhhHhHBB")      # 24 bytes

assert GNSS_STRUCT.size == 32
assert IMU_STRUCT.size == 24

CROCKFORD_BASE32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
EARTH_RADIUS_M = 6_371_000

# ============================================================================
# Waypoint pool -- Santa Monica / West LA area
# ============================================================================

Waypoint = Tuple[str, float, float]

WAYPOINTS: List[Waypoint] = [
    ("Santa Monica Pier",        34.0094, -118.4973),
    ("Ocean & Colorado",         34.0137, -118.4976),
    ("Ocean & Wilshire",         34.0195, -118.4912),
    ("Montana & 7th",            34.0302, -118.4928),
    ("Lincoln & Wilshire",       34.0262, -118.4710),
    ("Wilshire & Bundy",         34.0371, -118.4531),
    ("Venice & Lincoln",         33.9970, -118.4592),
    ("Main & Rose",              33.9941, -118.4744),
    ("Olympic & 26th",           34.0225, -118.4770),
    ("PCH & Temescal",           34.0450, -118.5250),
    ("Lincoln & Montana",        34.0305, -118.4750),
    ("Pico & Lincoln",           34.0135, -118.4600),
    ("26th & San Vicente",       34.0345, -118.4720),
    ("Clover Park",              34.0165, -118.4640),
    ("Bergamot Station",         34.0255, -118.4665),
    ("I-10 & Lincoln On-Ramp",   34.0200, -118.4580),
    ("I-10 & Centinela",         34.0170, -118.4370),
    ("I-10 & Robertson",         34.0290, -118.3870),
    ("I-10 & La Cienega",        34.0330, -118.3700),
    ("I-10 & La Brea",           34.0380, -118.3440),
    ("I-10 & Crenshaw",          34.0280, -118.3270),
    ("I-10 & Western",           34.0300, -118.3090),
    ("Rest Stop (fictional)",    34.0350, -118.2950),
    ("I-10 & Vermont",           34.0330, -118.2920),
]

# Home location for Wi-Fi sync
HOME_LOCATION = ("Home (Montana & 7th)", 34.0302, -118.4928)

# Underground garage (no GNSS)
GARAGE_LOCATION = ("Underground Garage", 34.0200, -118.4580)

# ============================================================================
# Math helpers
# ============================================================================

def clamp_i16(v: float) -> int:
    return max(-32768, min(32767, int(v)))

def clamp_u16(v: float) -> int:
    return max(0, min(65535, int(v)))

def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in metres."""
    r1, r2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = (math.sin((r2 - r1) / 2) ** 2
         + math.cos(r1) * math.cos(r2) * math.sin(dl / 2) ** 2)
    return EARTH_RADIUS_M * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

def bearing_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Initial bearing from point 1 to point 2, degrees [0, 360)."""
    r1, r2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    x = math.sin(dl) * math.cos(r2)
    y = math.cos(r1) * math.sin(r2) - math.sin(r1) * math.cos(r2) * math.cos(dl)
    return math.degrees(math.atan2(x, y)) % 360.0

def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t

def generate_ulid(rng: random.Random, timestamp_ms: int) -> str:
    """26-char ULID: 10 chars timestamp (Crockford base32) + 16 chars random."""
    ts = timestamp_ms & 0xFFFFFFFFFFFF
    chars = []
    for i in range(9, -1, -1):
        chars.append(CROCKFORD_BASE32[(ts >> (i * 5)) & 0x1F])
    chars.extend(rng.choice(CROCKFORD_BASE32) for _ in range(16))
    return "".join(chars)

# ============================================================================
# Device state enum
# ============================================================================

class DeviceState(Enum):
    SLEEP = auto()
    ARMING = auto()
    RECORDING = auto()
    STOP_CANDIDATE = auto()
    FINALIZING = auto()
    QUEUED_FOR_HOME_SYNC = auto()
    SYNCING = auto()
    RETAINED = auto()
    PRUNABLE = auto()
    LOW_BATTERY_PROTECTION = auto()
    FAULT = auto()

# ============================================================================
# GNSS sample / IMU summary as typed dicts for internal use
# ============================================================================

class GNSSSample:
    __slots__ = (
        "timestamp_ms", "latitude", "longitude", "altitude_cm",
        "speed_cmps", "heading_cdeg", "fix_quality", "satellites",
        "hdop_tenths", "accuracy_cm",
    )
    def __init__(
        self,
        timestamp_ms: int = 0,
        latitude: int = 0,
        longitude: int = 0,
        altitude_cm: int = 0,
        speed_cmps: int = 0,
        heading_cdeg: int = 0,
        fix_quality: int = 0,
        satellites: int = 0,
        hdop_tenths: int = 0,
        accuracy_cm: int = 0,
    ):
        self.timestamp_ms = timestamp_ms
        self.latitude = latitude
        self.longitude = longitude
        self.altitude_cm = altitude_cm
        self.speed_cmps = speed_cmps
        self.heading_cdeg = heading_cdeg
        self.fix_quality = fix_quality
        self.satellites = satellites
        self.hdop_tenths = hdop_tenths
        self.accuracy_cm = accuracy_cm

    def pack(self) -> bytes:
        return GNSS_STRUCT.pack(
            self.timestamp_ms,
            self.latitude,
            self.longitude,
            self.altitude_cm,
            clamp_u16(self.speed_cmps),
            clamp_u16(self.heading_cdeg),
            self.fix_quality,
            self.satellites,
            clamp_u16(self.hdop_tenths),
            clamp_u16(self.accuracy_cm),
            b"\x00\x00",
        )

    def speed_kmh(self) -> float:
        return self.speed_cmps * 0.036

    def lat_deg(self) -> float:
        return self.latitude / 1e7

    def lon_deg(self) -> float:
        return self.longitude / 1e7


class IMUSummary:
    __slots__ = (
        "window_start_ms", "window_duration_ms",
        "accel_peak_x_mg", "accel_peak_y_mg", "accel_peak_z_mg",
        "accel_rms_mg", "gyro_peak_dps", "variance", "flags",
    )
    def __init__(self, **kw: Any):
        self.window_start_ms: int = kw.get("window_start_ms", 0)
        self.window_duration_ms: int = kw.get("window_duration_ms", 20)
        self.accel_peak_x_mg: int = kw.get("accel_peak_x_mg", 0)
        self.accel_peak_y_mg: int = kw.get("accel_peak_y_mg", 0)
        self.accel_peak_z_mg: int = kw.get("accel_peak_z_mg", 1000)
        self.accel_rms_mg: int = kw.get("accel_rms_mg", 1000)
        self.gyro_peak_dps: int = kw.get("gyro_peak_dps", 0)
        self.variance: int = kw.get("variance", 0)
        self.flags: int = kw.get("flags", 0)

    def pack(self) -> bytes:
        return IMU_STRUCT.pack(
            self.window_start_ms,
            clamp_u16(self.window_duration_ms),
            clamp_i16(self.accel_peak_x_mg),
            clamp_i16(self.accel_peak_y_mg),
            clamp_i16(self.accel_peak_z_mg),
            clamp_u16(self.accel_rms_mg),
            clamp_i16(self.gyro_peak_dps),
            clamp_u16(self.variance),
            self.flags & 0xFF,
            0,
        )


# ============================================================================
# Route profiles
# ============================================================================

class RouteSegment:
    """A segment between two waypoints with driving parameters."""
    def __init__(
        self,
        start: Waypoint,
        end: Waypoint,
        cruise_kmh: float,
        stop_at_end_s: float = 0,
        profile: str = "city",
        gnss_override: Optional[str] = None,
    ):
        self.start = start
        self.end = end
        self.cruise_kmh = cruise_kmh
        self.stop_at_end_s = stop_at_end_s
        self.profile = profile  # city, highway, parking, mountain
        self.gnss_override = gnss_override  # "none", "degraded", None


class RouteProfile:
    """A complete route made of segments."""
    def __init__(self, name: str, segments: List[RouteSegment],
                 home_at_end: bool = True):
        self.name = name
        self.segments = segments
        self.home_at_end = home_at_end


# ============================================================================
# GNSS Emulator
# ============================================================================

class GNSSEmulator:
    """Generates realistic GNSS fixes along a route with quality variations."""

    def __init__(self, rng: random.Random, cold_start: bool = False):
        self.rng = rng
        self._fix_acquired = False
        self._fix_start_delay_ms = 0
        self._fix_acquired_at_ms = 0
        self._current_lat = 0.0
        self._current_lon = 0.0
        self._current_alt_cm = 3000
        self._current_speed_mps = 0.0
        self._current_heading = 0.0
        self._satellites = 0
        self._hdop_tenths = 99
        self._accuracy_cm = 9999
        self._gnss_blocked = False  # tunnel / garage
        self._degraded = False

        if cold_start:
            self._fix_start_delay_ms = rng.randint(30_000, 45_000)
        else:
            self._fix_start_delay_ms = rng.randint(5_000, 10_000)

    def set_position(self, lat: float, lon: float) -> None:
        self._current_lat = lat
        self._current_lon = lon

    def set_blocked(self, blocked: bool) -> None:
        """Simulate tunnel / garage GNSS blockage."""
        self._gnss_blocked = blocked
        if blocked:
            self._fix_acquired = False

    def set_degraded(self, degraded: bool) -> None:
        """Simulate urban canyon with poor fix quality."""
        self._degraded = degraded

    def update(
        self,
        elapsed_ms: int,
        target_lat: float,
        target_lon: float,
        target_speed_mps: float,
        target_heading: float,
        dt_ms: int,
    ) -> GNSSSample:
        """Generate a GNSS sample for the current moment."""
        sample = GNSSSample()
        sample.timestamp_ms = elapsed_ms

        # Blocked -- no fix
        if self._gnss_blocked:
            sample.fix_quality = 0
            sample.satellites = 0
            sample.hdop_tenths = 99
            sample.accuracy_cm = 9999
            sample.latitude = 0
            sample.longitude = 0
            return sample

        # Fix acquisition delay
        if not self._fix_acquired:
            if elapsed_ms < self._fix_start_delay_ms:
                sample.fix_quality = 0
                sample.satellites = self.rng.randint(0, 3)
                sample.hdop_tenths = 99
                sample.accuracy_cm = 9999
                return sample
            else:
                self._fix_acquired = True
                self._fix_acquired_at_ms = elapsed_ms
                self._satellites = self.rng.randint(6, 8)  # initial weak fix

        # Time since fix acquired -- quality improves over 10-20s
        fix_age_ms = elapsed_ms - self._fix_acquired_at_ms
        fix_maturity = min(1.0, fix_age_ms / 15_000)

        # Update position (smooth interpolation toward target)
        alpha = min(1.0, dt_ms / 500.0)
        self._current_lat = lerp(self._current_lat, target_lat, alpha)
        self._current_lon = lerp(self._current_lon, target_lon, alpha)
        self._current_speed_mps = lerp(
            self._current_speed_mps, target_speed_mps, alpha
        )
        self._current_heading = target_heading

        # Satellite count stabilises over time
        target_sats = 10 if not self._degraded else self.rng.randint(4, 7)
        self._satellites = int(lerp(
            self._satellites, target_sats, fix_maturity * 0.3
        ))
        self._satellites = max(0, min(14, self._satellites))

        if self._degraded:
            base_hdop = self.rng.randint(25, 45)
            base_accuracy = self.rng.randint(800, 2000)
            noise_m = 0.00003
        else:
            base_hdop = int(lerp(30, 8, fix_maturity))
            base_accuracy = int(lerp(1500, 200, fix_maturity))
            noise_m = 0.000002 * (1.5 - fix_maturity)

        self._hdop_tenths = base_hdop + self.rng.randint(-2, 2)
        self._accuracy_cm = base_accuracy + self.rng.randint(-50, 50)

        # Apply GPS noise
        noisy_lat = self._current_lat + self.rng.gauss(0, noise_m)
        noisy_lon = self._current_lon + self.rng.gauss(0, noise_m)

        sample.latitude = int(round(noisy_lat * 1e7))
        sample.longitude = int(round(noisy_lon * 1e7))
        sample.altitude_cm = self._current_alt_cm + self.rng.randint(-50, 50)
        sample.speed_cmps = clamp_u16(int(self._current_speed_mps * 100))
        sample.heading_cdeg = clamp_u16(
            int(self._current_heading * 100) % 36000
        )
        sample.fix_quality = 1 if self._satellites >= MIN_SATELLITES else 0
        sample.satellites = self._satellites
        sample.hdop_tenths = clamp_u16(self._hdop_tenths)
        sample.accuracy_cm = clamp_u16(self._accuracy_cm)

        return sample


# ============================================================================
# IMU Emulator
# ============================================================================

class IMUEmulator:
    """Generates IMU summaries based on driving dynamics."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self._prev_speed_mps = 0.0
        self._prev_heading = 0.0

    def update(
        self,
        timestamp_ms: int,
        speed_mps: float,
        heading: float,
        dt_ms: int,
        road_profile: str = "city",
    ) -> IMUSummary:
        summary = IMUSummary()
        summary.window_start_ms = timestamp_ms
        summary.window_duration_ms = 20  # ~50 Hz window

        dt_s = max(0.001, dt_ms / 1000.0)

        # Longitudinal acceleration (x-axis, forward)
        accel_mps2 = (speed_mps - self._prev_speed_mps) / dt_s
        accel_x_mg = int(accel_mps2 / 9.81 * 1000)

        # Lateral acceleration from heading change (y-axis)
        heading_delta = heading - self._prev_heading
        if heading_delta > 180:
            heading_delta -= 360
        elif heading_delta < -180:
            heading_delta += 360
        yaw_rate_dps = heading_delta / dt_s
        # Lateral accel = v * omega (in rad/s)
        lat_accel_mps2 = speed_mps * math.radians(yaw_rate_dps)
        accel_y_mg = int(lat_accel_mps2 / 9.81 * 1000)

        # Z-axis: 1g gravity + road vibration
        road_noise_mg = {
            "city": 30,
            "highway": 20,
            "parking": 10,
            "mountain": 50,
        }.get(road_profile, 25)

        vibration = int(self.rng.gauss(0, road_noise_mg)) if speed_mps > 0.5 else 0
        accel_z_mg = 1000 + vibration

        # Speed bump / impact spike
        is_impact = False
        if road_profile in ("city", "mountain") and self.rng.random() < 0.002:
            accel_z_mg += self.rng.randint(500, 1500)
            is_impact = True

        # RMS magnitude
        rms = int(math.sqrt(
            accel_x_mg**2 + accel_y_mg**2 + accel_z_mg**2
        ))

        # Gyroscope peak (degrees/sec * 10)
        gyro_dps10 = int(yaw_rate_dps * 10)

        # Variance: deviation from 1g rest
        variance = max(0, rms - 1000 + abs(accel_x_mg) + abs(accel_y_mg))

        # Road vibration noise added to x and y
        accel_x_mg += int(self.rng.gauss(0, road_noise_mg * 0.5))
        accel_y_mg += int(self.rng.gauss(0, road_noise_mg * 0.5))

        # Event detection flags
        flags = 0
        if is_impact or rms > 3000:
            flags |= 0x01  # impact
        if accel_x_mg < -800:
            flags |= 0x02  # hard brake
        if abs(accel_y_mg) > 600:
            flags |= 0x04  # sharp turn

        summary.accel_peak_x_mg = clamp_i16(accel_x_mg)
        summary.accel_peak_y_mg = clamp_i16(accel_y_mg)
        summary.accel_peak_z_mg = clamp_i16(accel_z_mg)
        summary.accel_rms_mg = clamp_u16(rms)
        summary.gyro_peak_dps = clamp_i16(gyro_dps10)
        summary.variance = clamp_u16(variance)
        summary.flags = flags

        self._prev_speed_mps = speed_mps
        self._prev_heading = heading

        return summary


# ============================================================================
# Battery / Power Emulator
# ============================================================================

class BatteryEmulator:
    """Simulates OBD-II vehicle voltage."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self._voltage_mv = 12_600  # 12.6V at rest
        self._engine_running = False
        self._cranking = False
        self._forced_low = False

    def start_engine(self) -> None:
        """Simulate engine cranking then running."""
        self._cranking = True
        self._voltage_mv = self.rng.randint(10_800, 11_200)

    def engine_running(self) -> None:
        self._cranking = False
        self._engine_running = True
        self._voltage_mv = self.rng.randint(14_000, 14_400)

    def stop_engine(self) -> None:
        self._engine_running = False
        self._cranking = False
        self._voltage_mv = 12_600

    def force_low_battery(self) -> None:
        self._forced_low = True

    def force_power_loss(self) -> None:
        self._voltage_mv = 0

    def update(self, dt_ms: int) -> int:
        """Return current voltage in millivolts."""
        if self._forced_low:
            self._voltage_mv = max(
                self._voltage_mv - self.rng.randint(5, 15), 10_500
            )
            return self._voltage_mv

        if self._cranking:
            self._voltage_mv = self.rng.randint(10_500, 11_200)
        elif self._engine_running:
            self._voltage_mv = 14_200 + self.rng.randint(-200, 200)
        else:
            drift = self.rng.randint(-3, 1)
            self._voltage_mv = max(11_000, self._voltage_mv + drift)

        return self._voltage_mv

    @property
    def voltage_mv(self) -> int:
        return self._voltage_mv


# ============================================================================
# Storage Emulator
# ============================================================================

class StorageEmulator:
    """Virtual microSD card with capacity tracking."""

    def __init__(self, output_dir: Path, capacity_mb: int = 16_384):
        self.output_dir = output_dir
        self.capacity_bytes = capacity_mb * 1024 * 1024
        self.used_bytes = 0
        self._trip_dirs: Dict[str, Path] = {}
        self.output_dir.mkdir(parents=True, exist_ok=True)

    @property
    def free_mb(self) -> int:
        return (self.capacity_bytes - self.used_bytes) // (1024 * 1024)

    @property
    def is_full(self) -> bool:
        return self.free_mb < MIN_FREE_SD_MB

    def set_nearly_full(self, free_mb: int = 45) -> None:
        self.used_bytes = self.capacity_bytes - free_mb * 1024 * 1024

    def open_trip(self, trip_id: str) -> Path:
        trip_dir = self.output_dir / trip_id
        trip_dir.mkdir(parents=True, exist_ok=True)
        self._trip_dirs[trip_id] = trip_dir
        return trip_dir

    def write_file(self, trip_id: str, filename: str, data: bytes) -> bool:
        if self.used_bytes + len(data) > self.capacity_bytes:
            return False
        trip_dir = self._trip_dirs.get(trip_id)
        if trip_dir is None:
            return False
        path = trip_dir / filename
        path.write_bytes(data)
        self.used_bytes += len(data)
        return True

    def prune_trip(self, trip_id: str) -> None:
        trip_dir = self._trip_dirs.pop(trip_id, None)
        if trip_dir and trip_dir.exists():
            for f in trip_dir.iterdir():
                sz = f.stat().st_size
                f.unlink()
                self.used_bytes = max(0, self.used_bytes - sz)
            trip_dir.rmdir()

    def get_trip_dir(self, trip_id: str) -> Optional[Path]:
        return self._trip_dirs.get(trip_id)


# ============================================================================
# Network Emulator
# ============================================================================

class NetworkEmulator:
    """Simulates Wi-Fi and the chunked upload protocol."""

    def __init__(
        self,
        rng: random.Random,
        server_url: str,
        device_id: str,
        latency_ms: int = 50,
    ):
        self.rng = rng
        self.server_url = server_url.rstrip("/") if server_url else ""
        self.device_id = device_id
        self.latency_ms = latency_ms
        self._connected = False
        self._force_drop = False
        self._drop_after_bytes = -1

    @property
    def connected(self) -> bool:
        return self._connected and not self._force_drop

    def scan_for_home(self) -> bool:
        """Simulate Wi-Fi scan for trusted SSID."""
        return True  # In emulation we are always near home when asked

    def connect(self) -> bool:
        self._connected = True
        self._force_drop = False
        return True

    def disconnect(self) -> None:
        self._connected = False

    def force_drop_after(self, byte_offset: int) -> None:
        """Schedule a Wi-Fi drop after uploading this many bytes."""
        self._drop_after_bytes = byte_offset
        self._force_drop = False

    def force_drop_now(self) -> None:
        self._force_drop = True
        self._connected = False

    def upload_bundle(
        self,
        trip_id: str,
        bundle_dir: Path,
        log_fn: Callable[[str], None],
    ) -> bool:
        """Execute the full chunked upload protocol. Returns True on success."""
        if not self.server_url:
            log_fn(f"[NET] No server configured -- skipping upload for {trip_id}")
            return True  # Treat as success for headless testing

        samples_path = bundle_dir / "samples.bin"
        if not samples_path.exists():
            log_fn(f"[NET] samples.bin not found in {bundle_dir}")
            return False

        data = samples_path.read_bytes()
        content_hash = hashlib.sha256(data).hexdigest()

        try:
            # Step 1: POST /api/v1/upload/init
            init_payload = json.dumps({
                "device_id": self.device_id,
                "trip_id": trip_id,
                "content_hash": content_hash,
                "size": len(data),
            }).encode()

            req = urllib.request.Request(
                f"{self.server_url}/api/v1/upload/init",
                data=init_payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=30) as resp:
                resp_body = json.loads(resp.read().decode())

            upload_id = resp_body["upload_id"]
            resume_offset = resp_body.get("resume_offset", 0)

            if resume_offset == -1:
                log_fn(f"[NET] Trip {trip_id} already uploaded (server confirms)")
                return True

            log_fn(
                f"[NET] Init OK: upload_id={upload_id}, "
                f"resume={resume_offset}, size={len(data)}"
            )

            # Step 2: PUT chunks
            offset = resume_offset
            while offset < len(data):
                if not self.connected:
                    log_fn(f"[NET] Wi-Fi dropped at offset {offset}")
                    return False

                if (self._drop_after_bytes >= 0
                        and offset >= self._drop_after_bytes):
                    log_fn(
                        f"[NET] Simulated Wi-Fi drop at offset {offset}"
                    )
                    self._force_drop = True
                    self._connected = False
                    return False

                chunk = data[offset:offset + UPLOAD_CHUNK_SIZE]
                chunk_req = urllib.request.Request(
                    f"{self.server_url}/api/v1/upload/{upload_id}/chunk",
                    data=chunk,
                    headers={
                        "Content-Type": "application/octet-stream",
                        "X-Upload-Offset": str(offset),
                    },
                    method="PUT",
                )
                with urllib.request.urlopen(chunk_req, timeout=30) as resp:
                    resp.read()

                offset += len(chunk)
                pct = min(100, int(offset / len(data) * 100))
                log_fn(f"[NET] Chunk: {offset}/{len(data)} ({pct}%)")

            # Step 3: POST finalize
            fin_payload = json.dumps({
                "content_hash": content_hash,
            }).encode()
            fin_req = urllib.request.Request(
                f"{self.server_url}/api/v1/upload/{upload_id}/finalize",
                data=fin_payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(fin_req, timeout=30) as resp:
                receipt = json.loads(resp.read().decode())

            log_fn(
                f"[NET] Finalized trip {trip_id}, "
                f"receipt={receipt.get('receipt_id', 'n/a')}"
            )
            return True

        except (urllib.error.URLError, OSError, json.JSONDecodeError) as exc:
            log_fn(f"[NET] Upload failed for {trip_id}: {exc}")
            return False


# ============================================================================
# Driving simulator -- interpolates along route segments
# ============================================================================

class DrivingSimulator:
    """Drives along a RouteProfile, producing speed + position at each tick."""

    def __init__(self, rng: random.Random, profile: RouteProfile):
        self.rng = rng
        self.profile = profile
        self.segments = list(profile.segments)
        self._seg_idx = 0
        self._seg_frac = 0.0
        self._seg_dist = 0.0
        self._seg_total_dist = 0.0
        self._current_speed_mps = 0.0
        self._finished = False
        self._stopped_timer_s = 0.0
        self._stop_duration_s = 0.0
        self._is_stopped_at_waypoint = False
        self._precompute_segment()

    def _precompute_segment(self) -> None:
        if self._seg_idx >= len(self.segments):
            self._finished = True
            return
        seg = self.segments[self._seg_idx]
        self._seg_total_dist = haversine_m(
            seg.start[1], seg.start[2], seg.end[1], seg.end[2]
        )
        self._seg_dist = 0.0
        self._seg_frac = 0.0
        self._is_stopped_at_waypoint = False
        self._stopped_timer_s = 0.0
        self._stop_duration_s = seg.stop_at_end_s

    @property
    def finished(self) -> bool:
        return self._finished

    @property
    def current_segment(self) -> Optional[RouteSegment]:
        if self._seg_idx < len(self.segments):
            return self.segments[self._seg_idx]
        return None

    def step(self, dt_s: float) -> Tuple[float, float, float, float, str]:
        """Advance simulation by dt_s. Returns (lat, lon, speed_mps, heading, profile)."""
        if self._finished:
            seg = self.segments[-1] if self.segments else None
            lat = seg.end[1] if seg else 0
            lon = seg.end[2] if seg else 0
            return lat, lon, 0.0, 0.0, "city"

        seg = self.segments[self._seg_idx]

        # Stopped at waypoint
        if self._is_stopped_at_waypoint:
            self._stopped_timer_s += dt_s
            if self._stopped_timer_s >= self._stop_duration_s:
                self._seg_idx += 1
                self._precompute_segment()
                if self._finished:
                    return seg.end[1], seg.end[2], 0.0, 0.0, seg.profile
            lat = seg.end[1] + self.rng.gauss(0, 0.000001)
            lon = seg.end[2] + self.rng.gauss(0, 0.000001)
            return lat, lon, 0.0, 0.0, seg.profile

        cruise_mps = seg.cruise_kmh / 3.6
        total = self._seg_total_dist
        if total < 1.0:
            total = 1.0

        # Trapezoidal speed profile
        accel_rate = self.rng.uniform(1.8, 2.5)
        decel_rate = self.rng.uniform(2.5, 3.5)
        t_accel = cruise_mps / accel_rate
        d_accel = 0.5 * accel_rate * t_accel**2
        t_decel = cruise_mps / decel_rate
        d_decel = 0.5 * decel_rate * t_decel**2

        if d_accel + d_decel > total:
            # Triangular: cap speed
            peak = math.sqrt(total / (0.5 / accel_rate + 0.5 / decel_rate))
            d_accel = 0.5 * accel_rate * (peak / accel_rate)**2
            d_decel = total - d_accel
            cruise_mps = peak

        d = self._seg_dist
        remaining = total - d

        # Compute target speed based on position in segment
        if d < d_accel:
            target_speed = math.sqrt(2 * accel_rate * max(d, 0.1))
        elif remaining < d_decel:
            target_speed = math.sqrt(2 * decel_rate * max(remaining, 0.1))
        else:
            target_speed = cruise_mps

        target_speed = max(0.0, min(cruise_mps, target_speed))

        # Smooth acceleration toward target speed
        speed_diff = target_speed - self._current_speed_mps
        max_delta = (accel_rate if speed_diff > 0 else decel_rate) * dt_s
        delta = max(-max_delta, min(max_delta, speed_diff))
        self._current_speed_mps = max(0.0, self._current_speed_mps + delta)

        # Advance position
        self._seg_dist += self._current_speed_mps * dt_s
        self._seg_frac = min(1.0, self._seg_dist / total)

        lat = lerp(seg.start[1], seg.end[1], self._seg_frac)
        lon = lerp(seg.start[2], seg.end[2], self._seg_frac)
        heading = bearing_deg(seg.start[1], seg.start[2], seg.end[1], seg.end[2])

        # Check if we reached segment end
        if self._seg_frac >= 1.0:
            self._current_speed_mps = 0.0
            if self._stop_duration_s > 0:
                self._is_stopped_at_waypoint = True
                self._stopped_timer_s = 0.0
            else:
                self._seg_idx += 1
                self._precompute_segment()

        return lat, lon, self._current_speed_mps, heading, seg.profile


# ============================================================================
# Trip Event Recorder
# ============================================================================

class EventRecorder:
    """Accumulates trip events for serialisation."""

    def __init__(self) -> None:
        self.events: List[Dict[str, Any]] = []

    def add(
        self, event_type: str, timestamp_ms: int,
        lat: float = 0.0, lon: float = 0.0, details: str = "",
    ) -> None:
        ts_dt = datetime.fromtimestamp(
            timestamp_ms / 1000.0, tz=timezone.utc
        )
        self.events.append({
            "type": event_type,
            "timestamp": ts_dt.isoformat(),
            "data": {"details": details, "lat": lat, "lon": lon},
        })


# ============================================================================
# Device Emulator -- full state machine
# ============================================================================

class DeviceEmulator:
    """Emulates one Freematics ONE+ device through its full lifecycle."""

    def __init__(
        self,
        device_id: str,
        rng: random.Random,
        output_dir: Path,
        server_url: str = "",
        speedup: float = 1.0,
        verbose: bool = False,
        headless: bool = True,
    ):
        self.device_id = device_id
        self.rng = rng
        self.output_dir = output_dir
        self.speedup = speedup
        self.verbose = verbose
        self.headless = headless

        self.state = DeviceState.SLEEP
        self._state_entered_ms = 0
        self._sim_time_ms = int(time.time() * 1000)
        self._wall_start = time.monotonic()

        self.gnss = GNSSEmulator(rng)
        self.imu = IMUEmulator(rng)
        self.battery = BatteryEmulator(rng)
        self.storage = StorageEmulator(output_dir)
        self.network = NetworkEmulator(rng, server_url, device_id)

        self._driving: Optional[DrivingSimulator] = None
        self._route_profile: Optional[RouteProfile] = None

        # Trip state
        self._trip_id = ""
        self._trip_start_ms = 0
        self._gnss_samples: List[GNSSSample] = []
        self._imu_summaries: List[IMUSummary] = []
        self._events = EventRecorder()
        self._gnss_sample_count = 0
        self._imu_summary_count = 0
        self._last_gnss_ms = 0
        self._last_imu_ms = 0

        # Queued (synced-but-retained) trips
        self._queued_trips: List[str] = []
        self._synced_trips: List[str] = []
        self._total_uploaded: int = 0
        self._all_trips: List[Dict[str, Any]] = []

        # Fault injection
        self._force_power_loss = False
        self._power_loss_at_state: Optional[DeviceState] = None
        self._force_wifi_drop = False
        self._force_low_battery = False
        self._force_storage_full = False

        # Lifecycle control
        self._shutdown = False
        self._last_wifi_scan_ms = 0

        # Logging
        self._log_lines: List[str] = []

    # -- Logging --

    def log(self, msg: str) -> None:
        ts = datetime.fromtimestamp(
            self._sim_time_ms / 1000, tz=timezone.utc
        ).strftime("%H:%M:%S.%f")[:-3]
        line = f"[{ts}] [{self.device_id}] {msg}"
        self._log_lines.append(line)
        if self.verbose or not self.headless:
            print(line, file=sys.stderr)

    # -- State transition --

    def _transition(self, new_state: DeviceState) -> None:
        old = self.state
        self.log(f"[STATE] {old.name} -> {new_state.name}")
        self.state = new_state
        self._state_entered_ms = self._sim_time_ms

    # -- Time helpers --

    def _state_elapsed_ms(self) -> int:
        return self._sim_time_ms - self._state_entered_ms

    def _advance_time(self, real_dt_ms: int) -> int:
        """Advance simulation time, return sim dt in ms."""
        sim_dt = int(real_dt_ms * self.speedup)
        self._sim_time_ms += sim_dt
        return sim_dt

    # -- Main run loop --

    def run_scenario(self, scenario: "Scenario") -> Dict[str, Any]:
        """Execute a complete scenario and return a summary dict."""
        self.log(f"=== Starting scenario: {scenario.name} ===")
        self._expects_errors = scenario.expects_errors

        # Apply scenario setup
        scenario.setup(self)

        tick_interval_ms = 200  # 200ms real-time tick -> scaled by speedup

        while not self._shutdown:
            sim_dt = self._advance_time(tick_interval_ms)

            # Apply scheduled scenario events
            scenario.tick(self, self._sim_time_ms)

            # Battery check (global, matches firmware)
            if (self.state not in (DeviceState.SLEEP, DeviceState.LOW_BATTERY_PROTECTION)
                    and self.battery.voltage_mv < LOW_BATTERY_THRESHOLD_MV):
                self._transition(DeviceState.LOW_BATTERY_PROTECTION)

            # State handlers
            if self.state == DeviceState.SLEEP:
                self._handle_sleep(sim_dt)
            elif self.state == DeviceState.ARMING:
                self._handle_arming(sim_dt)
            elif self.state == DeviceState.RECORDING:
                self._handle_recording(sim_dt)
            elif self.state == DeviceState.STOP_CANDIDATE:
                self._handle_stop_candidate(sim_dt)
            elif self.state == DeviceState.FINALIZING:
                self._handle_finalizing()
            elif self.state == DeviceState.QUEUED_FOR_HOME_SYNC:
                self._handle_queued(sim_dt)
            elif self.state == DeviceState.SYNCING:
                self._handle_syncing()
            elif self.state == DeviceState.RETAINED:
                self._handle_retained()
            elif self.state == DeviceState.PRUNABLE:
                self._handle_prunable()
            elif self.state == DeviceState.LOW_BATTERY_PROTECTION:
                self._handle_low_battery()
            elif self.state == DeviceState.FAULT:
                self._handle_fault()

            self.battery.update(sim_dt)

            # Scenario may request shutdown
            if scenario.is_complete(self, self._sim_time_ms):
                break

            # Real-time sleep (adjusted for speedup)
            if self.speedup < 100:
                time.sleep(tick_interval_ms / 1000.0)

        self.log(f"=== Scenario complete: {scenario.name} ===")
        return self._build_summary(scenario.name)

    # -- State handlers (mirror firmware state_machine.cpp) --

    def _handle_sleep(self, dt_ms: int) -> None:
        # In emulation, SLEEP just waits for a driving simulator to be set
        if self._driving is not None and not self._driving.finished:
            self.log("[SLEEP] Motion detected -> ARMING")
            self.battery.start_engine()
            # Short cranking then running
            self.battery.engine_running()
            self._transition(DeviceState.ARMING)

    def _handle_arming(self, dt_ms: int) -> None:
        elapsed = self._state_elapsed_ms()

        if self._driving is None or self._driving.finished:
            if elapsed > ARMING_DURATION_MS:
                self.log("[ARMING] No sustained movement -> SLEEP")
                self._transition(DeviceState.SLEEP)
            return

        # Sub-step at 1-second intervals so we don't miss speed changes
        # during large time leaps at high speedup.
        sub_step = min(1000, dt_ms)
        remaining = dt_ms
        while remaining > 0:
            chunk = min(sub_step, remaining)
            remaining -= chunk

            lat, lon, speed_mps, heading, profile = self._driving.step(
                chunk / 1000.0
            )

            if self._driving.finished:
                break

        speed_kmh = speed_mps * 3.6

        gnss_sample = self.gnss.update(
            self._sim_time_ms, lat, lon, speed_mps, heading, dt_ms,
        )
        imu_sample = self.imu.update(
            self._sim_time_ms, speed_mps, heading, dt_ms, profile,
        )

        conditions_met = (
            speed_kmh >= MIN_SPEED_KMH
            and (gnss_sample.speed_kmh() >= MIN_SPEED_KMH
                 or imu_sample.variance > ULP_MOTION_THRESHOLD_MG)
        )

        if not conditions_met:
            if elapsed > ARMING_DURATION_MS:
                self.log("[ARMING] Conditions not sustained -> SLEEP")
                self._transition(DeviceState.SLEEP)
            return

        if elapsed >= ARMING_DURATION_MS:
            # Begin recording
            self._trip_id = generate_ulid(self.rng, self._sim_time_ms)
            self._trip_start_ms = self._sim_time_ms
            self._gnss_samples = []
            self._imu_summaries = []
            self._events = EventRecorder()
            self._gnss_sample_count = 0
            self._imu_summary_count = 0
            self._last_gnss_ms = 0
            self._last_imu_ms = 0

            if self.storage.is_full:
                self.log("[ARMING] Storage full -- cannot record")
                self._events.add(
                    "storage_pressure", self._sim_time_ms,
                    details="SD card full",
                )
                self._transition(DeviceState.FAULT)
                return

            self.storage.open_trip(self._trip_id)
            self._events.add("trip_start", self._sim_time_ms, lat, lon,
                             "recording started")
            self.log(
                f"[ARMING] Trip started: {self._trip_id} "
                f"(speed={speed_kmh:.1f} km/h)"
            )
            self._transition(DeviceState.RECORDING)

    def _handle_recording(self, dt_ms: int) -> None:
        if self._force_power_loss and self._power_loss_at_state == DeviceState.RECORDING:
            self.log("[RECORDING] POWER LOSS -- data may be incomplete")
            self._events.add("power_anomaly", self._sim_time_ms,
                             details="power loss during recording")
            self._handle_finalizing()
            self._shutdown = True
            return

        elapsed = self._state_elapsed_ms()
        if elapsed >= MAX_TRIP_DURATION_MS:
            self.log("[RECORDING] Max trip duration reached")
            self._events.add("trip_end", self._sim_time_ms,
                             details="max duration")
            self._transition(DeviceState.FINALIZING)
            return

        if self._driving is None:
            return

        # Sub-step at GNSS rate.  For each GNSS sample we also emit
        # the correct number of IMU summaries (IMU_RATE / GNSS_RATE).
        gnss_interval = (1000 // GNSS_RATE_ACTIVE_HZ
                         if GNSS_RATE_ACTIVE_HZ else dt_ms)
        imu_per_gnss = (IMU_RATE_ACTIVE_HZ // GNSS_RATE_ACTIVE_HZ
                        if GNSS_RATE_ACTIVE_HZ else 1)
        imu_window_ms = (1000 // IMU_RATE_ACTIVE_HZ
                         if IMU_RATE_ACTIVE_HZ else 20)

        remaining = dt_ms
        while remaining > 0:
            sub_dt = min(gnss_interval, remaining)
            remaining -= sub_dt

            lat, lon, speed_mps, heading, profile = self._driving.step(
                sub_dt / 1000.0
            )
            speed_kmh = speed_mps * 3.6
            sample_ts = self._sim_time_ms - remaining

            # GNSS sample
            if sample_ts - self._last_gnss_ms >= gnss_interval:
                sample = self.gnss.update(
                    sample_ts, lat, lon, speed_mps, heading, sub_dt,
                )
                self._gnss_samples.append(sample)
                self._gnss_sample_count += 1
                self._last_gnss_ms = sample_ts

                # Emit batch of IMU summaries for this GNSS interval
                for k in range(imu_per_gnss):
                    imu_ts = sample_ts - gnss_interval + k * imu_window_ms
                    imu = self.imu.update(
                        imu_ts, speed_mps, heading, imu_window_ms, profile,
                    )
                    self._imu_summaries.append(imu)
                    self._imu_summary_count += 1
                self._last_imu_ms = sample_ts

                if speed_kmh < MIN_SPEED_KMH:
                    last_imu = self._imu_summaries[-1]
                    if last_imu.variance <= ULP_MOTION_THRESHOLD_MG:
                        self._events.add(
                            "stop_candidate", sample_ts, lat, lon,
                            "speed below threshold",
                        )
                        self._transition(DeviceState.STOP_CANDIDATE)
                        return

    def _handle_stop_candidate(self, dt_ms: int) -> None:
        elapsed = self._state_elapsed_ms()

        if self._driving is None:
            if elapsed >= STOP_DWELL_MS:
                self._events.add("trip_end", self._sim_time_ms,
                                 details="dwell timeout")
                self._transition(DeviceState.FINALIZING)
            return

        # Sub-step at slow GNSS rate
        gnss_interval = (1000 // GNSS_RATE_SLOW_HZ
                         if GNSS_RATE_SLOW_HZ else dt_ms)
        step_ms = gnss_interval
        remaining = dt_ms
        while remaining > 0:
            sub_dt = min(step_ms, remaining)
            remaining -= sub_dt
            sample_ts = self._sim_time_ms - remaining

            lat, lon, speed_mps, heading, profile = self._driving.step(
                sub_dt / 1000.0
            )
            speed_kmh = speed_mps * 3.6

            if sample_ts - self._last_gnss_ms >= gnss_interval:
                sample = self.gnss.update(
                    sample_ts, lat, lon, speed_mps, heading, sub_dt,
                )
                self._gnss_samples.append(sample)
                self._gnss_sample_count += 1
                self._last_gnss_ms = sample_ts

                imu = self.imu.update(
                    sample_ts, speed_mps, heading, sub_dt, profile,
                )
                self._imu_summaries.append(imu)
                self._imu_summary_count += 1

                if (speed_kmh >= MIN_SPEED_KMH
                        or imu.variance > ULP_MOTION_THRESHOLD_MG):
                    self.log("[STOP_CANDIDATE] Motion resumed -> RECORDING")
                    self._events.add(
                        "trip_resumed", sample_ts, lat, lon,
                        "motion resumed",
                    )
                    self._transition(DeviceState.RECORDING)
                    return

        elapsed = self._state_elapsed_ms()
        if elapsed >= STOP_DWELL_MS:
            self.log("[STOP_CANDIDATE] Dwell timeout -> FINALIZING")
            self._events.add("trip_end", self._sim_time_ms,
                             details="dwell timeout")
            self._transition(DeviceState.FINALIZING)

    def _handle_finalizing(self) -> None:
        if not self._gnss_samples:
            self.log("[FINALIZING] No samples -- nothing to finalize")
            self._transition(DeviceState.QUEUED_FOR_HOME_SYNC)
            return

        ok = self._write_trip_bundle()
        if ok:
            self.log(
                f"[FINALIZING] Bundle written: {self._gnss_sample_count} GNSS, "
                f"{self._imu_summary_count} IMU"
            )
            self._queued_trips.append(self._trip_id)
            self._transition(DeviceState.QUEUED_FOR_HOME_SYNC)
        else:
            self.log("[FINALIZING] Failed to write bundle")
            self._transition(DeviceState.FAULT)

    def _handle_queued(self, dt_ms: int) -> None:
        # Check if there is another trip to record
        if (self._driving is not None and not self._driving.finished):
            # We have more driving to do -- re-arm for next trip
            self._transition(DeviceState.ARMING)
            return

        # Scan for Wi-Fi
        if self._sim_time_ms - self._last_wifi_scan_ms >= WIFI_SCAN_INTERVAL_MS:
            self._last_wifi_scan_ms = self._sim_time_ms

            if self.network.scan_for_home():
                if self.network.connect():
                    self.log("[QUEUED] Connected to home Wi-Fi -> SYNCING")
                    self._transition(DeviceState.SYNCING)
                    return

        # Give up after 10 scan cycles
        if self._state_elapsed_ms() > WIFI_SCAN_INTERVAL_MS * 10:
            self.log("[QUEUED] No network -- going to SLEEP")
            self._transition(DeviceState.SLEEP)

    def _handle_syncing(self) -> None:
        if self._force_power_loss and self._power_loss_at_state == DeviceState.SYNCING:
            self.log("[SYNCING] POWER LOSS during sync")
            self.network.disconnect()
            self._shutdown = True
            return

        if not self._queued_trips:
            self.log("[SYNCING] All trips synced")
            self.network.disconnect()
            self._transition(DeviceState.RETAINED)
            return

        trip_id = self._queued_trips[0]
        bundle_dir = self.storage.get_trip_dir(trip_id)
        if bundle_dir is None:
            self.log(f"[SYNCING] Trip dir not found for {trip_id}")
            self._queued_trips.pop(0)
            return

        ok = self.network.upload_bundle(trip_id, bundle_dir, self.log)
        if ok:
            self._queued_trips.pop(0)
            self._synced_trips.append(trip_id)
            self._total_uploaded += 1
            self.log(f"[SYNCING] Upload complete for {trip_id}")
            if not self._queued_trips:
                self.network.disconnect()
                self._transition(DeviceState.RETAINED)
        else:
            self.log(f"[SYNCING] Upload failed for {trip_id} -- requeue")
            self.network.disconnect()
            self._transition(DeviceState.QUEUED_FOR_HOME_SYNC)

    def _handle_retained(self) -> None:
        # In real firmware, wait RETENTION_WINDOW_HOURS then prune.
        # In emulation, fast-forward through retention.
        self.log("[RETAINED] Trip data retained (retention window skipped in emulation)")
        self._transition(DeviceState.PRUNABLE)

    def _handle_prunable(self) -> None:
        for trip_id in self._synced_trips:
            # In emulation, keep files on disk for test inspection.
            # Real firmware would delete here.
            self.log(f"[PRUNABLE] Trip {trip_id} marked prunable (files retained for testing)")
        self._synced_trips.clear()
        self.battery.stop_engine()
        self._transition(DeviceState.SLEEP)
        self._shutdown = True

    def _handle_low_battery(self) -> None:
        self.log("[LOW_BATTERY] Finalizing trip if active, entering deep sleep")
        self._events.add("power_anomaly", self._sim_time_ms,
                         details="low battery protection")
        if self._gnss_samples:
            self._write_trip_bundle()
            self._queued_trips.append(self._trip_id)
        self._shutdown = True

    def _handle_fault(self) -> None:
        self.log("[FAULT] Error state -- device would reboot")
        self._shutdown = True

    # -- Bundle writing --

    def _write_trip_bundle(self) -> bool:
        """Write trip bundle to storage in exact firmware format."""
        trip_id = self._trip_id

        # Pack GNSS samples
        samples_buf = bytearray()
        for s in self._gnss_samples:
            samples_buf.extend(s.pack())

        # Pack IMU summaries
        imu_buf = bytearray()
        for s in self._imu_summaries:
            imu_buf.extend(s.pack())

        # Events JSON
        events_bytes = json.dumps(
            self._events.events, indent=2
        ).encode("utf-8")

        # Manifest
        ended_ms = self._sim_time_ms
        started_at = datetime.fromtimestamp(
            self._trip_start_ms / 1000, tz=timezone.utc
        ).isoformat()
        ended_at = datetime.fromtimestamp(
            ended_ms / 1000, tz=timezone.utc
        ).isoformat()
        manifest = {
            "version": 1,
            "trip_id": trip_id,
            "device_id": self.device_id,
            "firmware_version": "0.1.0-emulator",
            "started_at": started_at,
            "ended_at": ended_at,
            "started_at_ms": self._trip_start_ms,
            "ended_at_ms": ended_ms,
            "sample_count": {
                "gnss": self._gnss_sample_count,
                "imu_summary": self._imu_summary_count,
                "imu_raw_windows": 0,
            },
            "gnss_sample_count": self._gnss_sample_count,
            "imu_summary_count": self._imu_summary_count,
            "gnss_rate_hz": GNSS_RATE_ACTIVE_HZ,
            "imu_rate_hz": IMU_RATE_ACTIVE_HZ,
            "schema_version": 1,
            "compression": "none",
        }
        manifest_bytes = json.dumps(manifest, indent=2).encode("utf-8")

        # Checksums
        samples_hash = hashlib.sha256(samples_buf).hexdigest()
        imu_hash = hashlib.sha256(imu_buf).hexdigest()
        events_hash = hashlib.sha256(events_bytes).hexdigest()
        manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()

        sums_text = "\n".join([
            f"{samples_hash}  samples.bin",
            f"{imu_hash}  imu_summary.bin",
            f"{events_hash}  events.json",
            f"{manifest_hash}  manifest.json",
        ]) + "\n"

        # Write files
        ok = True
        ok = ok and self.storage.write_file(trip_id, "samples.bin", bytes(samples_buf))
        ok = ok and self.storage.write_file(trip_id, "imu_summary.bin", bytes(imu_buf))
        ok = ok and self.storage.write_file(trip_id, "events.json", events_bytes)
        ok = ok and self.storage.write_file(trip_id, "manifest.json", manifest_bytes)
        ok = ok and self.storage.write_file(trip_id, "sha256sums.txt",
                                            sums_text.encode("utf-8"))

        if ok:
            duration_s = (ended_ms - self._trip_start_ms) / 1000
            self._all_trips.append({
                "trip_id": trip_id,
                "gnss_samples": self._gnss_sample_count,
                "imu_summaries": self._imu_summary_count,
                "events": len(self._events.events),
                "duration_s": duration_s,
                "samples_bytes": len(samples_buf),
                "imu_bytes": len(imu_buf),
            })

        return ok

    def _build_summary(self, scenario_name: str) -> Dict[str, Any]:
        return {
            "scenario": scenario_name,
            "device_id": self.device_id,
            "trips_generated": len(self._all_trips),
            "trips_uploaded": self._total_uploaded,
            "trips_queued": len(self._queued_trips),
            "final_state": self.state.name,
            "trips": self._all_trips,
            "errors": [l for l in self._log_lines if "FAULT" in l or "failed" in l.lower()],
            "expects_errors": getattr(self, "_expects_errors", False),
            "total_gnss_samples": sum(t["gnss_samples"] for t in self._all_trips),
            "total_imu_summaries": sum(t["imu_summaries"] for t in self._all_trips),
        }


# ============================================================================
# Scenario base class and built-in scenarios
# ============================================================================

class Scenario:
    """Base class for test scenarios."""

    name: str = "base"
    description: str = ""
    # Max simulation time before auto-completion (ms).
    # Default 6 hours; scenarios override as needed.
    max_sim_duration_ms: int = 6 * 3600 * 1000
    # Set True for scenarios that intentionally trigger faults/failures.
    expects_errors: bool = False

    def setup(self, device: DeviceEmulator) -> None:
        """Configure the device before the run loop starts."""
        pass

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        """Called each tick to inject scheduled events."""
        pass

    def is_complete(self, device: DeviceEmulator, sim_time_ms: int) -> bool:
        """Return True when the scenario is done."""
        if device._shutdown:
            return True
        # Auto-complete if the device is in a terminal state with nothing
        # left to do (driving finished, no queued trips).
        if (device.state == DeviceState.SLEEP
                and (device._driving is None or device._driving.finished)
                and not device._queued_trips):
            return True
        # Safety timeout
        elapsed = sim_time_ms - device._sim_time_ms + (
            device._sim_time_ms - device._state_entered_ms
            if device.state == DeviceState.SLEEP else 0
        )
        return False


def _make_city_route(
    rng: random.Random,
    waypoint_indices: List[int],
    cruise_range: Tuple[float, float] = (25, 45),
    stops: int = 3,
) -> RouteProfile:
    """Build a city route from waypoint indices with random stops."""
    segments: List[RouteSegment] = []
    stop_indices = set()
    interior = list(range(1, len(waypoint_indices) - 1))
    if interior and stops > 0:
        stop_indices = set(rng.sample(interior, min(stops, len(interior))))

    for i in range(len(waypoint_indices) - 1):
        start_wp = WAYPOINTS[waypoint_indices[i]]
        end_wp = WAYPOINTS[waypoint_indices[i + 1]]
        cruise = rng.uniform(*cruise_range)
        stop = rng.uniform(15, 60) if i in stop_indices else 0
        segments.append(RouteSegment(start_wp, end_wp, cruise, stop, "city"))

    return RouteProfile("city_route", segments, home_at_end=True)


def _make_highway_route(rng: random.Random) -> RouteProfile:
    """Build a highway route: city -> highway -> rest stop -> highway -> return."""
    segments = [
        # City to on-ramp
        RouteSegment(WAYPOINTS[3], WAYPOINTS[4], rng.uniform(30, 45), 0, "city"),
        RouteSegment(WAYPOINTS[4], WAYPOINTS[15], rng.uniform(35, 50), 0, "city"),
        # Highway segments
        RouteSegment(WAYPOINTS[15], WAYPOINTS[16], rng.uniform(90, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[16], WAYPOINTS[17], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[17], WAYPOINTS[18], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[18], WAYPOINTS[19], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[19], WAYPOINTS[20], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[20], WAYPOINTS[21], rng.uniform(95, 115), 0, "highway"),
        # Rest stop
        RouteSegment(WAYPOINTS[21], WAYPOINTS[22], rng.uniform(40, 60),
                     rng.uniform(300, 600), "city"),
        # Return highway
        RouteSegment(WAYPOINTS[22], WAYPOINTS[21], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[21], WAYPOINTS[20], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[20], WAYPOINTS[19], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[19], WAYPOINTS[18], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[18], WAYPOINTS[17], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[17], WAYPOINTS[16], rng.uniform(95, 115), 0, "highway"),
        RouteSegment(WAYPOINTS[16], WAYPOINTS[15], rng.uniform(80, 100), 0, "highway"),
        # Off-ramp back to city
        RouteSegment(WAYPOINTS[15], WAYPOINTS[4], rng.uniform(30, 45), 0, "city"),
        RouteSegment(WAYPOINTS[4], WAYPOINTS[3], rng.uniform(30, 40), 0, "city"),
    ]
    return RouteProfile("highway_trip", segments, home_at_end=True)


# -- Scenario implementations --

class NormalCommuteScenario(Scenario):
    name = "normal_commute"
    description = "25-min city drive, park at destination, Wi-Fi sync at home"

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_city_route(
            device.rng,
            [0, 1, 2, 3, 10, 4, 5, 4, 10, 3],
            cruise_range=(25, 45),
            stops=4,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[0][1], WAYPOINTS[0][2])
        device.battery.start_engine()
        device.battery.engine_running()


class HighwayTripScenario(Scenario):
    name = "highway_trip"
    description = "2-hour highway drive with rest stop"

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_highway_route(device.rng)
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()


class ShortErrandScenario(Scenario):
    name = "short_errand"
    description = "3-min trip to corner store (should trigger, not be filtered)"

    def setup(self, device: DeviceEmulator) -> None:
        segments = [
            RouteSegment(WAYPOINTS[3], WAYPOINTS[10],
                         device.rng.uniform(20, 35), 0, "city"),
        ]
        route = RouteProfile("short_errand", segments, home_at_end=True)
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()


class GarageStartScenario(Scenario):
    name = "garage_start"
    description = "Start in underground garage (no GNSS), drive out, get fix"

    def __init__(self) -> None:
        super().__init__()
        self._exit_time_ms = 0
        self._setup_done = False

    def setup(self, device: DeviceEmulator) -> None:
        device.gnss = GNSSEmulator(device.rng, cold_start=True)
        device.gnss.set_blocked(True)  # No GNSS in garage
        segments = [
            RouteSegment(GARAGE_LOCATION, WAYPOINTS[4],
                         device.rng.uniform(15, 25), 0, "parking",
                         gnss_override="none"),
            RouteSegment(WAYPOINTS[4], WAYPOINTS[5],
                         device.rng.uniform(30, 45), 0, "city"),
            RouteSegment(WAYPOINTS[5], WAYPOINTS[3],
                         device.rng.uniform(30, 40), 0, "city"),
        ]
        route = RouteProfile("garage_start", segments, home_at_end=True)
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(GARAGE_LOCATION[1], GARAGE_LOCATION[2])
        device.battery.start_engine()
        device.battery.engine_running()
        self._exit_time_ms = device._sim_time_ms + 30_000  # 30s to exit garage

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        if sim_time_ms >= self._exit_time_ms and not self._setup_done:
            device.gnss.set_blocked(False)
            device.log("[SCENARIO] Exited garage -- GNSS unblocked")
            self._setup_done = True


class MultiStopErrandsScenario(Scenario):
    name = "multi_stop_errands"
    description = "4 stops in 90 minutes (separate trips or merged?)"

    def setup(self, device: DeviceEmulator) -> None:
        segments = [
            RouteSegment(WAYPOINTS[3], WAYPOINTS[10],
                         device.rng.uniform(25, 40),
                         device.rng.uniform(180, 300), "city"),
            RouteSegment(WAYPOINTS[10], WAYPOINTS[4],
                         device.rng.uniform(25, 40),
                         device.rng.uniform(180, 300), "city"),
            RouteSegment(WAYPOINTS[4], WAYPOINTS[11],
                         device.rng.uniform(25, 40),
                         device.rng.uniform(180, 300), "city"),
            RouteSegment(WAYPOINTS[11], WAYPOINTS[13],
                         device.rng.uniform(25, 40),
                         device.rng.uniform(180, 300), "city"),
            RouteSegment(WAYPOINTS[13], WAYPOINTS[3],
                         device.rng.uniform(25, 40), 0, "city"),
        ]
        route = RouteProfile("multi_stop", segments, home_at_end=True)
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()


class PowerLossRecordingScenario(Scenario):
    name = "power_loss_recording"
    description = "Power yanked during active recording"
    expects_errors = True

    def __init__(self) -> None:
        super().__init__()
        self._loss_triggered = False

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_city_route(
            device.rng, [0, 1, 2, 3, 10, 4],
            cruise_range=(30, 50), stops=2,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[0][1], WAYPOINTS[0][2])
        device.battery.start_engine()
        device.battery.engine_running()
        # Do NOT set _force_power_loss yet -- wait for samples

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        if (device.state == DeviceState.RECORDING
                and device._gnss_sample_count > 50
                and not self._loss_triggered):
            self._loss_triggered = True
            device._force_power_loss = True
            device._power_loss_at_state = DeviceState.RECORDING
            device.log("[SCENARIO] Power loss triggered NOW")


class PowerLossUploadScenario(Scenario):
    name = "power_loss_upload"
    description = "Wi-Fi drops during sync, resume on reconnect"
    expects_errors = True

    def __init__(self) -> None:
        super().__init__()
        self._drop_triggered = False
        self._reconnect_time_ms = 0

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_city_route(
            device.rng, [3, 10, 4, 3],
            cruise_range=(25, 40), stops=1,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()
        # Schedule Wi-Fi drop after 2KB uploaded
        device.network.force_drop_after(2048)

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        if (device.state == DeviceState.QUEUED_FOR_HOME_SYNC
                and self._drop_triggered
                and sim_time_ms >= self._reconnect_time_ms):
            device.network._force_drop = False
            device.network._drop_after_bytes = -1
            device.log("[SCENARIO] Wi-Fi reconnected -- retry upload")

        if (device.state == DeviceState.QUEUED_FOR_HOME_SYNC
                and not self._drop_triggered
                and device.network._force_drop):
            self._drop_triggered = True
            self._reconnect_time_ms = sim_time_ms + 10_000
            device.log("[SCENARIO] Wi-Fi dropped -- will reconnect in 10s")


class LowBatteryScenario(Scenario):
    name = "low_battery"
    description = "Battery drops below threshold during recording"
    expects_errors = True

    def __init__(self) -> None:
        super().__init__()
        self._triggered = False

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_city_route(
            device.rng, [0, 1, 2, 3, 10, 4],
            cruise_range=(25, 40), stops=2,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[0][1], WAYPOINTS[0][2])
        device.battery.start_engine()
        device.battery.engine_running()

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        if (device.state == DeviceState.RECORDING
                and device._gnss_sample_count > 30
                and not self._triggered):
            device.battery.force_low_battery()
            self._triggered = True
            device.log("[SCENARIO] Battery dropping below threshold")


class StorageFullScenario(Scenario):
    name = "storage_full"
    description = "SD card nearly full, device must handle pressure"
    expects_errors = True

    def setup(self, device: DeviceEmulator) -> None:
        device.storage.set_nearly_full(free_mb=45)  # Below MIN_FREE_SD_MB
        # Longer route so ARMING has time to detect sustained motion
        route = _make_city_route(
            device.rng, [3, 10, 4, 5, 4, 10, 3],
            cruise_range=(25, 40), stops=2,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()


class LongParkScenario(Scenario):
    name = "long_park"
    description = "Device parked for 8 hours, ULP wake, no false triggers"

    def __init__(self) -> None:
        super().__init__()
        self._park_end_ms = 0

    def setup(self, device: DeviceEmulator) -> None:
        device.gnss.set_position(WAYPOINTS[5][1], WAYPOINTS[5][2])
        # 8 hours of parking -- no driving simulator
        self._park_end_ms = device._sim_time_ms + 8 * 3600 * 1000
        device.log("[SCENARIO] Parked for 8 hours -- no movement expected")

    def is_complete(self, device: DeviceEmulator, sim_time_ms: int) -> bool:
        return sim_time_ms >= self._park_end_ms or device._shutdown


class TunnelDriveScenario(Scenario):
    name = "tunnel_drive"
    description = "GNSS loss for 2 minutes mid-highway"

    def __init__(self) -> None:
        super().__init__()
        self._tunnel_start_ms = 0
        self._tunnel_end_ms = 0
        self._in_tunnel = False
        self._exited = False

    def setup(self, device: DeviceEmulator) -> None:
        segments = [
            RouteSegment(WAYPOINTS[3], WAYPOINTS[4],
                         device.rng.uniform(30, 45), 0, "city"),
            RouteSegment(WAYPOINTS[4], WAYPOINTS[15],
                         device.rng.uniform(40, 55), 0, "city"),
            RouteSegment(WAYPOINTS[15], WAYPOINTS[16],
                         device.rng.uniform(90, 110), 0, "highway"),
            RouteSegment(WAYPOINTS[16], WAYPOINTS[17],
                         device.rng.uniform(90, 110), 0, "highway"),
            RouteSegment(WAYPOINTS[17], WAYPOINTS[16],
                         device.rng.uniform(90, 110), 0, "highway"),
            RouteSegment(WAYPOINTS[16], WAYPOINTS[15],
                         device.rng.uniform(80, 100), 0, "highway"),
            RouteSegment(WAYPOINTS[15], WAYPOINTS[4],
                         device.rng.uniform(30, 45), 0, "city"),
            RouteSegment(WAYPOINTS[4], WAYPOINTS[3],
                         device.rng.uniform(25, 40), 0, "city"),
        ]
        route = RouteProfile("tunnel_drive", segments, home_at_end=True)
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        # Enter tunnel after some highway driving
        if (device.state == DeviceState.RECORDING
                and device._gnss_sample_count > 100
                and not self._in_tunnel and not self._exited):
            device.gnss.set_blocked(True)
            self._in_tunnel = True
            self._tunnel_start_ms = sim_time_ms
            self._tunnel_end_ms = sim_time_ms + 120_000  # 2 minutes
            device.log("[SCENARIO] Entering tunnel -- GNSS blocked")
            device._events.add(
                "gnss_quality_degraded", sim_time_ms,
                details="tunnel entry",
            )

        if self._in_tunnel and sim_time_ms >= self._tunnel_end_ms:
            device.gnss.set_blocked(False)
            self._in_tunnel = False
            self._exited = True
            device.log("[SCENARIO] Exiting tunnel -- GNSS restored")
            device._events.add(
                "gnss_quality_restored", sim_time_ms,
                details="tunnel exit",
            )


class MultiDeviceScenario(Scenario):
    """Not used directly -- the CLI handles multi-device via threading."""
    name = "multi_device"
    description = "3 emulated devices uploading simultaneously"

    def setup(self, device: DeviceEmulator) -> None:
        route = _make_city_route(
            device.rng, [0, 1, 2, 3, 10, 4, 5],
            cruise_range=(25, 45), stops=3,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[0][1], WAYPOINTS[0][2])
        device.battery.start_engine()
        device.battery.engine_running()


class RapidTripsScenario(Scenario):
    name = "rapid_trips"
    description = "10 trips in quick succession (valet parking scenario)"

    def __init__(self) -> None:
        super().__init__()
        self._trip_count = 0
        self._max_trips = 10

    def setup(self, device: DeviceEmulator) -> None:
        self._schedule_next_trip(device)

    def _schedule_next_trip(self, device: DeviceEmulator) -> None:
        if self._trip_count >= self._max_trips:
            return

        rng = device.rng
        # Short routes around parking area
        wp_pool = [0, 1, 2, 7, 8, 11, 13, 14]
        start = rng.choice(wp_pool)
        end = rng.choice([w for w in wp_pool if w != start])

        segments = [
            RouteSegment(
                WAYPOINTS[start], WAYPOINTS[end],
                rng.uniform(10, 25), 0, "parking",
            ),
        ]
        route = RouteProfile(f"rapid_trip_{self._trip_count}", segments)
        device._driving = DrivingSimulator(rng, route)
        device.gnss.set_position(
            WAYPOINTS[start][1], WAYPOINTS[start][2]
        )
        self._trip_count += 1

    def tick(self, device: DeviceEmulator, sim_time_ms: int) -> None:
        # When driving is done and we've finalized, schedule next trip
        if (device.state == DeviceState.QUEUED_FOR_HOME_SYNC
                and self._trip_count < self._max_trips):
            device.log(
                f"[SCENARIO] Scheduling rapid trip "
                f"{self._trip_count + 1}/{self._max_trips}"
            )
            self._schedule_next_trip(device)

    def is_complete(self, device: DeviceEmulator, sim_time_ms: int) -> bool:
        return (
            device._shutdown
            or (self._trip_count >= self._max_trips
                and device.state in (
                    DeviceState.SLEEP, DeviceState.PRUNABLE,
                    DeviceState.RETAINED,
                ))
        )


class DegradedGNSSScenario(Scenario):
    name = "degraded_gnss"
    description = "Driving in urban canyon with intermittent fix quality"

    def setup(self, device: DeviceEmulator) -> None:
        device.gnss.set_degraded(True)
        route = _make_city_route(
            device.rng, [2, 4, 5, 12, 10, 3],
            cruise_range=(20, 40), stops=3,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[2][1], WAYPOINTS[2][2])
        device.battery.start_engine()
        device.battery.engine_running()


class ColdStartScenario(Scenario):
    name = "cold_start"
    description = "Device first boot, no stored state, cold GNSS fix"

    def setup(self, device: DeviceEmulator) -> None:
        device.gnss = GNSSEmulator(device.rng, cold_start=True)
        route = _make_city_route(
            device.rng, [3, 10, 4, 5, 4, 3],
            cruise_range=(25, 40), stops=2,
        )
        device._driving = DrivingSimulator(device.rng, route)
        device.gnss.set_position(WAYPOINTS[3][1], WAYPOINTS[3][2])
        device.battery.start_engine()
        device.battery.engine_running()


# Scenario registry
SCENARIOS: Dict[str, type] = {
    "normal_commute": NormalCommuteScenario,
    "highway_trip": HighwayTripScenario,
    "short_errand": ShortErrandScenario,
    "garage_start": GarageStartScenario,
    "multi_stop_errands": MultiStopErrandsScenario,
    "power_loss_recording": PowerLossRecordingScenario,
    "power_loss_upload": PowerLossUploadScenario,
    "low_battery": LowBatteryScenario,
    "storage_full": StorageFullScenario,
    "long_park": LongParkScenario,
    "tunnel_drive": TunnelDriveScenario,
    "multi_device": MultiDeviceScenario,
    "rapid_trips": RapidTripsScenario,
    "degraded_gnss": DegradedGNSSScenario,
    "cold_start": ColdStartScenario,
}


# ============================================================================
# TUI Dashboard (optional curses)
# ============================================================================

def run_tui(device: DeviceEmulator, scenario: Scenario) -> Dict[str, Any]:
    """Run a scenario with a curses TUI showing live state."""
    try:
        import curses
    except ImportError:
        print("curses not available -- falling back to headless mode",
              file=sys.stderr)
        return device.run_scenario(scenario)

    result: Dict[str, Any] = {}

    def _tui_main(stdscr: Any) -> None:
        nonlocal result
        curses.curs_set(0)
        stdscr.nodelay(True)
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_GREEN, -1)
        curses.init_pair(2, curses.COLOR_YELLOW, -1)
        curses.init_pair(3, curses.COLOR_RED, -1)
        curses.init_pair(4, curses.COLOR_CYAN, -1)

        # Run the scenario in a background thread
        result_holder: List[Optional[Dict[str, Any]]] = [None]

        def _run() -> None:
            result_holder[0] = device.run_scenario(scenario)

        thread = threading.Thread(target=_run, daemon=True)
        thread.start()

        while thread.is_alive():
            try:
                stdscr.clear()
                h, w = stdscr.getmaxyx()

                # Title
                title = f" Cairn Emulator -- {scenario.name} "
                stdscr.addstr(0, max(0, (w - len(title)) // 2), title,
                              curses.A_BOLD | curses.color_pair(4))

                # State
                state_color = {
                    DeviceState.SLEEP: 1,
                    DeviceState.RECORDING: 2,
                    DeviceState.FAULT: 3,
                    DeviceState.LOW_BATTERY_PROTECTION: 3,
                }.get(device.state, 4)

                row = 2
                stdscr.addstr(row, 1, f"Device:   {device.device_id}")
                row += 1
                stdscr.addstr(row, 1, f"State:    {device.state.name}",
                              curses.color_pair(state_color) | curses.A_BOLD)
                row += 1
                stdscr.addstr(
                    row, 1,
                    f"Battery:  {device.battery.voltage_mv / 1000:.2f} V"
                )
                row += 1
                stdscr.addstr(
                    row, 1,
                    f"Storage:  {device.storage.free_mb} MB free"
                )
                row += 1

                # Trip info
                if device._trip_id:
                    stdscr.addstr(row, 1, f"Trip:     {device._trip_id}")
                    row += 1
                    stdscr.addstr(
                        row, 1,
                        f"GNSS:     {device._gnss_sample_count} samples"
                    )
                    row += 1
                    stdscr.addstr(
                        row, 1,
                        f"IMU:      {device._imu_summary_count} summaries"
                    )
                    row += 1

                # Last GNSS fix
                if device._gnss_samples:
                    last = device._gnss_samples[-1]
                    row += 1
                    stdscr.addstr(row, 1, "--- Last GNSS Fix ---")
                    row += 1
                    stdscr.addstr(
                        row, 1,
                        f"Lat: {last.lat_deg():.7f}  "
                        f"Lon: {last.lon_deg():.7f}"
                    )
                    row += 1
                    stdscr.addstr(
                        row, 1,
                        f"Speed: {last.speed_kmh():.1f} km/h  "
                        f"Sats: {last.satellites}  "
                        f"HDOP: {last.hdop_tenths / 10:.1f}"
                    )
                    row += 1

                # Upload status
                row += 1
                stdscr.addstr(row, 1, f"Queued:   {len(device._queued_trips)}")
                row += 1
                stdscr.addstr(row, 1, f"Synced:   {len(device._synced_trips)}")
                row += 1

                # Recent log lines
                row += 1
                stdscr.addstr(row, 1, "--- Log ---",
                              curses.A_BOLD)
                row += 1
                max_lines = max(0, h - row - 2)
                recent = device._log_lines[-max_lines:]
                for line in recent:
                    if row >= h - 1:
                        break
                    display = line[:w - 2]
                    try:
                        stdscr.addstr(row, 1, display)
                    except curses.error:
                        pass
                    row += 1

                stdscr.refresh()
                time.sleep(0.1)

                # Check for quit key
                key = stdscr.getch()
                if key in (ord('q'), ord('Q'), 27):  # q or ESC
                    device._shutdown = True

            except curses.error:
                pass

        result = result_holder[0] or {}

    curses.wrapper(_tui_main)
    return result


# ============================================================================
# Multi-device runner
# ============================================================================

def run_multi_device(
    scenario_cls: type,
    device_count: int,
    server_url: str,
    output_dir: Path,
    speedup: float,
    verbose: bool,
    seed: Optional[int],
) -> List[Dict[str, Any]]:
    """Run multiple devices concurrently using threads."""
    results: List[Optional[Dict[str, Any]]] = [None] * device_count
    threads: List[threading.Thread] = []

    for i in range(device_count):
        dev_id = f"cairn-emulator-{i + 1:02d}"
        dev_seed = (seed + i) if seed is not None else None
        dev_rng = random.Random(dev_seed)
        dev_output = output_dir / dev_id
        dev_output.mkdir(parents=True, exist_ok=True)

        device = DeviceEmulator(
            device_id=dev_id,
            rng=dev_rng,
            output_dir=dev_output,
            server_url=server_url,
            speedup=speedup,
            verbose=verbose,
            headless=True,
        )
        scenario = scenario_cls()

        def _run(d: DeviceEmulator, s: Scenario, idx: int) -> None:
            results[idx] = d.run_scenario(s)

        t = threading.Thread(target=_run, args=(device, scenario, i))
        threads.append(t)

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    return [r for r in results if r is not None]


# ============================================================================
# CLI
# ============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Cairn Freematics ONE+ Model B device emulator. "
            "Simulates the full device lifecycle for testing."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join([
            "Available scenarios:",
            *[f"  {name:25s} {cls.description}"
              for name, cls in sorted(SCENARIOS.items())],
        ]),
    )
    parser.add_argument(
        "--scenario", type=str, default=None,
        choices=list(SCENARIOS.keys()),
        help="Scenario to run",
    )
    parser.add_argument(
        "--all-scenarios", action="store_true",
        help="Run all built-in scenarios sequentially",
    )
    parser.add_argument(
        "--server", type=str, default="",
        help="Ingest server URL (e.g., http://localhost:8443)",
    )
    parser.add_argument(
        "--device-id", type=str, default="cairn-emulator-01",
        help="Device identifier (default: cairn-emulator-01)",
    )
    parser.add_argument(
        "--speedup", type=float, default=10.0,
        help="Time speedup factor (default: 10)",
    )
    parser.add_argument(
        "--devices", type=int, default=1,
        help="Number of concurrent devices (default: 1)",
    )
    parser.add_argument(
        "--headless", action="store_true",
        help="Disable TUI, print to stdout only",
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Print all log lines to stderr",
    )
    parser.add_argument(
        "--output", type=str, default=None,
        help="Output directory (default: emulator/output/)",
    )
    parser.add_argument(
        "--seed", type=int, default=None,
        help="Random seed for reproducibility",
    )

    args = parser.parse_args()

    # Determine output directory
    script_dir = Path(__file__).resolve().parent
    if args.output:
        output_dir = Path(args.output)
    else:
        output_dir = script_dir / "output"
    output_dir.mkdir(parents=True, exist_ok=True)

    # Determine scenarios to run
    if args.all_scenarios:
        scenario_names = list(SCENARIOS.keys())
    elif args.scenario:
        scenario_names = [args.scenario]
    else:
        parser.error("Specify --scenario or --all-scenarios")
        return 1

    # Signal handling for graceful shutdown
    shutdown_event = threading.Event()

    def _signal_handler(sig: int, frame: Any) -> None:
        print("\nShutting down gracefully...", file=sys.stderr)
        shutdown_event.set()

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    all_results: List[Dict[str, Any]] = []
    exit_code = 0

    for scenario_name in scenario_names:
        if shutdown_event.is_set():
            break

        scenario_cls = SCENARIOS[scenario_name]
        print(f"\n{'='*60}", file=sys.stderr)
        print(f"  Scenario: {scenario_name}", file=sys.stderr)
        print(f"  {scenario_cls.description}", file=sys.stderr)
        print(f"{'='*60}", file=sys.stderr)

        if args.devices > 1 or scenario_name == "multi_device":
            count = max(args.devices, 3 if scenario_name == "multi_device" else 1)
            results = run_multi_device(
                scenario_cls, count, args.server, output_dir,
                args.speedup, args.verbose, args.seed,
            )
            all_results.extend(results)
        else:
            rng = random.Random(args.seed)
            dev_output = output_dir / args.device_id
            dev_output.mkdir(parents=True, exist_ok=True)

            device = DeviceEmulator(
                device_id=args.device_id,
                rng=rng,
                output_dir=dev_output,
                server_url=args.server,
                speedup=args.speedup,
                verbose=args.verbose,
                headless=args.headless,
            )

            # Wire shutdown signal
            orig_shutdown = device._shutdown

            def _check_shutdown() -> None:
                if shutdown_event.is_set():
                    device._shutdown = True

            scenario = scenario_cls()

            if not args.headless:
                result = run_tui(device, scenario)
            else:
                result = device.run_scenario(scenario)

            all_results.append(result)

        # Check for unexpected errors
        for r in all_results:
            if r.get("errors") and not r.get("expects_errors"):
                exit_code = 1

    # Print JSON summary
    summary = {
        "emulator_version": "1.0.0",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "speedup": args.speedup,
        "scenarios_run": len(all_results),
        "results": all_results,
        "total_trips": sum(r.get("trips_generated", 0) for r in all_results),
        "total_gnss_samples": sum(
            r.get("total_gnss_samples", 0) for r in all_results
        ),
        "total_imu_summaries": sum(
            r.get("total_imu_summaries", 0) for r in all_results
        ),
        "total_uploads": sum(
            r.get("trips_uploaded", 0) for r in all_results
        ),
        "total_errors": sum(len(r.get("errors", [])) for r in all_results),
        "output_dir": str(output_dir),
    }

    print(json.dumps(summary, indent=2))

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
