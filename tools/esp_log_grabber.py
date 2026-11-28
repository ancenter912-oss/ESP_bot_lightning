#!/usr/bin/env python3
"""
ESP8266 Diagnostic Log Grabber (USB Serial -> .txt file + optional Telegram upload to @korotych_dev_bot)

Usage:
  pip install pyserial
  python tools/esp_log_grabber.py --port COM3
  python tools/esp_log_grabber.py --port COM3 --live
"""

import argparse
import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

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


def load_telegram_config(cli_token=None, cli_chat_id=None):
    token = (cli_token or "").strip()
    chat_id = (cli_chat_id or "").strip()
    bot_name = "@korotych_dev_bot"

    secrets_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tester_secrets.json")
    if os.path.isfile(secrets_path):
        try:
            with open(secrets_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if not token:
                token = str(data.get("tg_bot_token", "")).strip()
            if not chat_id:
                chat_id = str(data.get("tg_chat_id", "")).strip()
            if data.get("bot_username"):
                bot_name = str(data["bot_username"]).strip()
        except (OSError, json.JSONDecodeError):
            pass

    if not token:
        token = os.environ.get("DEV_TG_BOT_TOKEN", "").strip()
    if not chat_id:
        chat_id = os.environ.get("DEV_TG_CHAT_ID", "").strip()

    if token.startswith("REPLACE_") or len(token) < 20:
        token = ""
    if chat_id.startswith("REPLACE_") or len(chat_id) < 3:
        chat_id = ""
    return token, chat_id, bot_name


def upload_to_telegram(file_path: str, token: str, chat_id: str, bot_name: str):
    if not token or not chat_id:
        return
    with open(file_path, "rb") as f:
        file_bytes = f.read()
    filename = os.path.basename(file_path)
    boundary = f"----EspGrabBoundary{uuid.uuid4().hex}"
    fields = {
        "chat_id": chat_id,
        "caption": f"🩺 <b>USB-дамп логов ESP8266</b> (<code>{filename}</code>)",
        "parse_mode": "HTML",
    }
    chunks = []
    for k, v in fields.items():
        chunks.append(f"--{boundary}\r\n".encode("utf-8"))
        chunks.append(f'Content-Disposition: form-data; name="{k}"\r\n\r\n'.encode("utf-8"))
        chunks.append(str(v).encode("utf-8"))
        chunks.append(b"\r\n")
    chunks.append(f"--{boundary}\r\n".encode("utf-8"))
    chunks.append(
        f'Content-Disposition: form-data; name="document"; filename="{filename}"\r\n'
        f"Content-Type: text/plain; charset=utf-8\r\n\r\n".encode("utf-8")
    )
    chunks.append(file_bytes)
    chunks.append(f"\r\n--{boundary}--\r\n".encode("utf-8"))

    req = urllib.request.Request(
        f"https://api.telegram.org/bot{token}/sendDocument",
        data=b"".join(chunks),
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            if 200 <= resp.status < 300:
                print(f"[TG] Дамп {filename} отправлен в {bot_name}.")
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"[WARN] Ошибка отправки в {bot_name}: {str(exc).replace(token, '***')}")


def main():
    parser = argparse.ArgumentParser(description="Grab ESP8266 persisted LittleFS diagnostics over USB Serial.")
    parser.add_argument("--port", help="Serial port (e.g. COM3 or /dev/ttyUSB0). Auto-detected if omitted.")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--out", help="Output file path (default: logs/esp_diag_<timestamp>.txt)")
    parser.add_argument("--live", action="store_true", help="Keep recording live Serial logs after DUMP until Ctrl+C")
    parser.add_argument("--tg-token", help="Telegram bot token for @korotych_dev_bot")
    parser.add_argument("--tg-chat-id", help="Target Telegram chat_id")
    parser.add_argument("--no-upload", action="store_true", help="Skip Telegram upload")
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

    abs_path = os.path.abspath(out_path)
    print(f"[OK] Saved diagnostics to: {abs_path}")
    if not args.no_upload:
        token, chat_id, bot_name = load_telegram_config(args.tg_token, args.tg_chat_id)
        upload_to_telegram(abs_path, token, chat_id, bot_name)


if __name__ == "__main__":
    main()
