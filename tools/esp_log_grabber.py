#!/usr/bin/env python3
"""
ESP8266 Diagnostic Log Grabber (USB Serial -> .txt file)

Usage:
  pip install pyserial
  python tools/esp_log_grabber.py --port COM3
  python tools/esp_log_grabber.py --port COM3 --live
"""

import argparse
import datetime
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial is not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def detect_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None
    for p in ports:
        desc = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in desc for k in ("ch340", "cp210", "usb-serial", "silicon labs", "wch")):
            return p.device
    return ports[0].device


def main():
    parser = argparse.ArgumentParser(description="Grab ESP8266 persisted LittleFS diagnostics over USB Serial.")
    parser.add_argument("--port", help="Serial port (e.g. COM3 or /dev/ttyUSB0). Auto-detected if omitted.")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--out", help="Output file path (default: logs/esp_diag_<timestamp>.txt)")
    parser.add_argument("--live", action="store_true", help="Keep recording live Serial logs after DUMP until Ctrl+C")
    args = parser.parse_args()

    port = args.port or detect_port()
    if not port:
        print("ERROR: No serial ports found. Connect ESP8266 via USB and specify --port COMx.", file=sys.stderr)
        sys.exit(1)

    os.makedirs("logs", exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = args.out or os.path.join("logs", f"esp_diag_{ts}.txt")

    print(f"[INFO] Opening {port} @ {args.baud} baud...")
    with serial.Serial(port, args.baud, timeout=0.5) as ser, open(out_path, "w", encoding="utf-8") as out:
        # Avoid toggling DTR/RTS reset if possible, wait briefly for port settle
        time.sleep(1.0)
        ser.reset_input_buffer()

        print("[INFO] Requesting DUMP from ESP8266 LittleFS...")
        ser.write(b"\nDUMP\n")
        ser.flush()

        end_marker = "----- END ESP8266 DIAG DUMP -----"
        deadline = time.time() + 12.0
        got_end = False

        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace")
            out.write(line)
            out.flush()
            sys.stdout.write(line)
            if end_marker in line:
                got_end = True
                break

        if not got_end:
            print("\n[WARN] End marker not received within 12s (saved all received output).", file=sys.stderr)

        if args.live:
            print(f"\n[INFO] Live recording to {out_path} (press Ctrl+C to stop)...")
            out.write("\n--- LIVE SERIAL CAPTURE ---\n")
            try:
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    now_str = datetime.datetime.now().strftime("%H:%M:%S")
                    line = f"[{now_str}] " + raw.decode("utf-8", errors="replace")
                    out.write(line)
                    out.flush()
                    sys.stdout.write(line)
            except KeyboardInterrupt:
                print("\n[INFO] Live capture stopped.")

    print(f"[OK] Saved diagnostics to: {os.path.abspath(out_path)}")


if __name__ == "__main__":
    main()
