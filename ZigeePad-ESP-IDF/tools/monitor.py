"""USB monitor that reconnects after ESP32-C6 deep sleep without resetting it.

Run: python tools/monitor.py COM3
Stop: Ctrl+C. Close the monitor before flashing.
"""
import sys
import time
from pathlib import Path
import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
    print(f'Micropad monitor: {port}, 115200 baud. Ctrl+C to stop.', flush=True)
    print('Waiting for USB. Wake a key (e.g. D2-D7 = button 7) to see logs.', flush=True)
    connection = None
    waiting = False
    log_path = Path(__file__).resolve().parents[1] / 'build' / 'live-monitor.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log = log_path.open('ab', buffering=0)
    log.write(b'\n--- Monitor session ---\n')
    print(f'Log: {log_path}', flush=True)

    def notice(message):
        line = f'[{time.strftime("%H:%M:%S")}] {message}'
        print(line, flush=True)
        log.write((line + '\n').encode('utf-8'))

    try:
        while True:
            try:
                if connection is None:
                    connection = serial.Serial(port=None, baudrate=115200, timeout=0.1)
                    # Do not reset the board or drive it into the bootloader on reconnect.
                    connection.dtr = False
                    connection.rts = False
                    connection.port = port
                    connection.open()
                    notice(f'USB connected: {port}; waiting for firmware output.')
                    waiting = False
                chunk = connection.read(connection.in_waiting or 1)
                if chunk:
                    log.write(chunk)
                    sys.stdout.write(chunk.decode('utf-8', errors='replace'))
                    sys.stdout.flush()
            except (serial.SerialException, OSError) as error:
                was_connected = connection is not None and connection.is_open
                if connection is not None:
                    try:
                        connection.close()
                    except (serial.SerialException, OSError) as close_error:
                        log.write(f'USB close detail: {close_error}\n'.encode('utf-8'))
                    connection = None
                if not waiting:
                    log.write(f'USB driver detail: {error}\n'.encode('utf-8'))
                    if was_connected:
                        notice('USB connection lost; reconnecting automatically. Deep sleep also disconnects USB.')
                    else:
                        notice(f'Waiting for {port}; port unavailable. Driver details saved to log.')
                    notice('USB status alone does not confirm sleep; check firmware STATE messages.')
                    waiting = True
                time.sleep(0.2)
    except KeyboardInterrupt:
        print('\nMonitor stopped.', flush=True)
    finally:
        if connection is not None:
            connection.close()
        log.close()


if __name__ == '__main__':
    main()
