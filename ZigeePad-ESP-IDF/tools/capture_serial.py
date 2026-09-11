"""Bounded serial capture; a USB disconnect during deep sleep is expected.
Usage: python tools/capture_serial.py COM3 30 build/power-status.log
"""
import sys
import time
from pathlib import Path
import serial

port, duration, destination = sys.argv[1:]
captured = bytearray()
connection = None
try:
    connection = serial.Serial(port, 115200, timeout=0.1)
    deadline = time.monotonic() + float(duration)
    while time.monotonic() < deadline:
        chunk = connection.read(connection.in_waiting or 1)
        captured.extend(chunk)
        print(chunk.decode('utf-8', errors='replace'), end='', flush=True)
except serial.SerialException as error:
    print(f'\nUSB connection ended: {error}')
finally:
    if connection is not None:
        connection.close()
    Path(destination).write_bytes(captured)
