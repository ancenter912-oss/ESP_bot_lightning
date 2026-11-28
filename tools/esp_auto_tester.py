#!/usr/bin/env python3
"""
Interactive Step-by-Step Automated Tester for Korotych ESP8266 Opto Sensor (v1.0.2+)

Usage:
  pip install pyserial
  python tools/esp_auto_tester.py
  python tools/esp_auto_tester.py --port COM3 --cycles 5
  python tools/esp_auto_tester.py --self-test
"""

import argparse
import datetime
import os
import re
import sys
import time
import urllib.error
import urllib.request

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None


HEAP_MIN_OK = 28000
HEAP_MIN_WARN = 15000
MAX_BLOCK_MIN_OK = 20000
MAX_BLOCK_MIN_CRIT = 16500
FRAG_MAX_OK = 20
FRAG_MAX_WARN = 35

RE_WIFI = re.compile(
    r"wifi_status:\s*(-?\d+)\s*\(ip=([^,]+),\s*rssi=(-?\d+)\s*dBm,\s*disc=(\d+),\s*last_disc_reason=(\d+)\)"
)
RE_HEAP = re.compile(r"heap_now:\s*(\d+)\s*B\s*\(min_seen=(\d+)\s*B\)")
RE_BLOCK = re.compile(r"max_block_now:\s*(\d+)\s*B\s*\(min_seen=(\d+)\s*B\)")
RE_FRAG = re.compile(r"frag_now:\s*(\d+)%\s*\(max_seen=(\d+)%\)")
RE_VLG = re.compile(r"village:\s*ok=(\d+)\s+fail=(\d+)\s+last_http=(-?\d+)\s+ssl_err='([^']*)'")
RE_TG = re.compile(
    r"telegram:\s*configured=(yes|no)\s+ok=(\d+)\s+fail=(\d+)\s+last_http=(-?\d+)\s+ssl_err='([^']*)'"
)
RE_LOG_LINE = re.compile(
    r"^\[(\d+)\]\[([A-Z]+)\]\s+(.*?)\s+\|\s+h=(\d+)\s+b=(\d+)\s+f=(\d+)%\s+rssi=(-?\d+)$"
)


def parse_status_block(text: str) -> dict:
    info = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("fw_version:"):
            info["fw_version"] = line.split(":", 1)[1].strip()
        elif line.startswith("sensor_id:"):
            info["sensor_id"] = line.split(":", 1)[1].strip()
        elif line.startswith("sensor_label:"):
            info["sensor_label"] = line.split(":", 1)[1].strip()
        elif line.startswith("uptime_sec:"):
            info["uptime_sec"] = int(line.split(":", 1)[1].strip())
        elif line.startswith("mains_220v:"):
            info["mains_220v"] = line.split(":", 1)[1].strip()
        elif line.startswith("reset_reason:"):
            info["reset_reason"] = line.split(":", 1)[1].strip()
        elif line.startswith("reset_info:"):
            info["reset_info"] = line.split(":", 1)[1].strip()
        else:
            m = RE_WIFI.match(line)
            if m:
                info["wifi_status"] = int(m.group(1))
                info["ip"] = m.group(2).strip()
                info["rssi"] = int(m.group(3))
                info["wifi_disc"] = int(m.group(4))
                info["wifi_last_disc_reason"] = int(m.group(5))
                continue
            m = RE_HEAP.match(line)
            if m:
                info["heap_now"] = int(m.group(1))
                info["heap_min"] = int(m.group(2))
                continue
            m = RE_BLOCK.match(line)
            if m:
                info["block_now"] = int(m.group(1))
                info["block_min"] = int(m.group(2))
                continue
            m = RE_FRAG.match(line)
            if m:
                info["frag_now"] = int(m.group(1))
                info["frag_max"] = int(m.group(2))
                continue
            m = RE_VLG.match(line)
            if m:
                info["vlg_ok"] = int(m.group(1))
                info["vlg_fail"] = int(m.group(2))
                info["vlg_http"] = int(m.group(3))
                info["vlg_ssl_err"] = m.group(4)
                continue
            m = RE_TG.match(line)
            if m:
                info["tg_configured"] = (m.group(1) == "yes")
                info["tg_ok"] = int(m.group(2))
                info["tg_fail"] = int(m.group(3))
                info["tg_http"] = int(m.group(4))
                info["tg_ssl_err"] = m.group(5)
    return info


def parse_diag_line(line: str):
    m = RE_LOG_LINE.match(line.strip())
    if not m:
        return None
    return {
        "uptime_sec": int(m.group(1)),
        "tag": m.group(2),
        "message": m.group(3),
        "heap": int(m.group(4)),
        "block": int(m.group(5)),
        "frag": int(m.group(6)),
        "rssi": int(m.group(7)),
    }


def evaluate_memory(heap: int, block: int, frag: int):
    issues = []
    status = "PASS"
    if heap < HEAP_MIN_WARN or block < MAX_BLOCK_MIN_CRIT or frag > FRAG_MAX_WARN:
        status = "FAIL"
    elif heap < HEAP_MIN_OK or block < MAX_BLOCK_MIN_OK or frag > FRAG_MAX_OK:
        status = "WARN"

    if heap < HEAP_MIN_OK:
        issues.append(f"FreeHeap={heap} B (норма >= {HEAP_MIN_OK} B)")
    if block < MAX_BLOCK_MIN_OK:
        issues.append(f"MaxFreeBlock={block} B (норма >= {MAX_BLOCK_MIN_OK} B для BearSSL 16KB)")
    if frag > FRAG_MAX_OK:
        issues.append(f"Fragmentation={frag}% (норма <= {FRAG_MAX_OK}%)")
    return status, issues


def select_serial_port(preferred_port=None):
    if serial is None:
        print("ОШИБКА: не установлена библиотека pyserial. Выполните: pip install pyserial", file=sys.stderr)
        sys.exit(1)

    if preferred_port:
        return preferred_port

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("ОШИБКА: COM-порты не найдены. Подключите ESP8266 дата-кабелем USB (проверьте драйвер CH340/CP210x).")
        sys.exit(1)

    if len(ports) == 1:
        print(f"[AUTO] Найден порт: {ports[0].device} ({ports[0].description})")
        return ports[0].device

    for p in ports:
        desc = f"{p.description} {p.manufacturer or ''}".lower()
        if any(k in desc for k in ("ch340", "cp210", "usb-serial", "silicon labs", "wch")):
            print(f"[AUTO] Выбран порт ESP8266: {p.device} ({p.description})")
            return p.device

    print("\nДоступные COM-порты:")
    for idx, p in enumerate(ports, 1):
        print(f"  {idx}. {p.device} — {p.description}")
    while True:
        choice = input("Введите номер порта (например, 1): ").strip()
        if choice.isdigit() and 1 <= int(choice) <= len(ports):
            return ports[int(choice) - 1].device


class EspTester:
    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.results = []
        self.raw_transcript = []
        self.heap_samples = []

    def open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.25)
        time.sleep(1.0)
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def record(self, step_name: str, status: str, details: str):
        self.results.append((step_name, status, details))
        mark = {"PASS": "[PASS]", "WARN": "[WARN]", "FAIL": "[FAIL]", "SKIP": "[SKIP]"}.get(status, f"[{status}]")
        print(f"  {mark} {step_name}: {details}")

    def read_line(self):
        raw = self.ser.readline()
        if not raw:
            return None
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if line:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            self.raw_transcript.append(f"[{ts}] {line}")
            parsed = parse_diag_line(line)
            if parsed:
                self.heap_samples.append((parsed["heap"], parsed["block"], parsed["frag"]))
        return line

    def query_status(self, timeout_sec: float = 5.0) -> dict:
        self.ser.reset_input_buffer()
        self.ser.write(b"\nSTATUS\n")
        self.ser.flush()
        lines = []
        deadline = time.time() + timeout_sec
        started = False
        while time.time() < deadline:
            line = self.read_line()
            if line is None:
                continue
            if "=== Korotych ESP8266 Diagnostics ===" in line:
                started = True
            if started:
                lines.append(line)
                if line.startswith("endpoints:"):
                    break
        return parse_status_block("\n".join(lines))

    def query_dump(self, timeout_sec: float = 12.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write(b"\nDUMP\n")
        self.ser.flush()
        lines = []
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            line = self.read_line()
            if line is None:
                continue
            lines.append(line)
            if "----- END ESP8266 DIAG DUMP -----" in line:
                break
        return "\n".join(lines)

    def wait_for_mains_change(self, target_state: str, timeout_sec: float = 35.0):
        """Wait for '[MAINS] Change -> 220V_ON/OFF' and collect follow-up VLG/TG responses."""
        expected = f"Change -> {target_state}"
        deadline = time.time() + timeout_sec
        mains_seen = False
        vlg_msg = None
        tg_msg = None
        follow_deadline = None

        while time.time() < deadline:
            line = self.read_line()
            if line is None:
                if follow_deadline and time.time() >= follow_deadline:
                    break
                continue
            parsed = parse_diag_line(line)
            if not parsed:
                continue
            print(f"    лог: [{parsed['tag']}] {parsed['message']} (h={parsed['heap']} b={parsed['block']} f={parsed['frag']}%)")
            if parsed["tag"] == "MAINS" and expected in parsed["message"]:
                mains_seen = True
                follow_deadline = min(deadline, time.time() + 12.0)
            elif mains_seen and parsed["tag"] == "VLG":
                vlg_msg = parsed["message"]
            elif mains_seen and parsed["tag"] == "TG":
                tg_msg = parsed["message"]

            if mains_seen and vlg_msg and tg_msg:
                break
            if follow_deadline and time.time() >= follow_deadline:
                break

        return mains_seen, vlg_msg, tg_msg

    def check_no_false_trigger(self, watch_sec: float = 6.0):
        deadline = time.time() + watch_sec
        false_events = []
        while time.time() < deadline:
            line = self.read_line()
            if not line:
                continue
            parsed = parse_diag_line(line)
            if parsed and parsed["tag"] == "MAINS" and "Change ->" in parsed["message"]:
                false_events.append(parsed["message"])
        return false_events

    def run_all(self, stress_cycles: int = 3):
        print("\n============================================================")
        print("  АВТОМАТИЧЕСКИЙ ТЕСТЕР ПЛАТЫ ESP8266 (Korotych v1.0.2+)")
        print("============================================================")

        # STEP 1: Handshake & STATUS
        print("\n[Шаг 1/7] Связь с платой, Wi-Fi и память (команда STATUS)...")
        status = self.query_status()
        if not status or "fw_version" not in status:
            self.record(
                "Шаг 1 — Связь с платой",
                "FAIL",
                "Плата не ответила на STATUS. Проверьте прошивку v1.0.2+, скорость 115200 и не открыт ли Serial Monitor.",
            )
            return status

        print(
            f"    Прошивка: v{status.get('fw_version')} | ID: {status.get('sensor_id')} | "
            f"Точка: {status.get('sensor_label')} | 220В: {status.get('mains_220v')}"
        )
        print(
            f"    Wi-Fi: status={status.get('wifi_status')} IP={status.get('ip')} "
            f"RSSI={status.get('rssi')} dBm | Рестарт: {status.get('reset_reason')}"
        )
        mem_status, mem_issues = evaluate_memory(
            status.get("heap_now", 0), status.get("block_now", 0), status.get("frag_now", 100)
        )
        if status.get("wifi_status") != 3:
            self.record("Шаг 1 — Связь и Wi-Fi", "FAIL", f"Wi-Fi не подключён (status={status.get('wifi_status')})")
        else:
            self.record(
                "Шаг 1 — Связь и Wi-Fi",
                "PASS",
                f"v{status.get('fw_version')}, IP={status.get('ip')}, RSSI={status.get('rssi')} dBm, TG={'вкл' if status.get('tg_configured') else 'выкл'}",
            )
        self.record(
            "Шаг 1б — Память на старте",
            mem_status,
            f"heap={status.get('heap_now')} B, maxBlk={status.get('block_now')} B, frag={status.get('frag_now')}%"
            + (f" ({'; '.join(mem_issues)})" if mem_issues else ""),
        )

        # STEP 2: HTTP /status and /diag
        print("\n[Шаг 2/7] Проверка локального веб-сервера диагностики (/status и /diag)...")
        ip = status.get("ip", "")
        if ip and ip != "0.0.0.0":
            try:
                with urllib.request.urlopen(f"http://{ip}/status", timeout=5) as r1:
                    body1 = r1.read().decode("utf-8", errors="replace")
                with urllib.request.urlopen(f"http://{ip}/diag", timeout=8) as r2:
                    body2 = r2.read().decode("utf-8", errors="replace")
                if "Korotych ESP8266 Diagnostics" in body1 and "PERSISTED LOGS" in body2:
                    self.record("Шаг 2 — HTTP /status и /diag", "PASS", f"http://{ip}/diag доступен ({len(body2)} байт)")
                else:
                    self.record("Шаг 2 — HTTP /status и /diag", "WARN", "Ответ получен, но формат неполный")
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                self.record(
                    "Шаг 2 — HTTP /status и /diag",
                    "WARN",
                    f"Не удалось открыть http://{ip}/diag с этого ПК ({exc}). Возможно, ПК и ESP в разных Wi-Fi сетях.",
                )
        else:
            self.record("Шаг 2 — HTTP /status и /diag", "SKIP", "Нет IP-адреса")

        # STEP 3 & 4: D5 -> ON and OFF
        current_mains = status.get("mains_220v", "OFF")
        if current_mains == "ON":
            print("\n[Шаг 3/7] Сейчас пин D5 замкнут (220V = ON). Сначала проверим ПРОПАДАНИЕ света.")
            steps_seq = [
                ("Шаг 3 — Пропадание 220 В", "220V_OFF", "РАЗОМКНИТЕ пин D5 от GND (оставьте в воздухе на 4+ сек)"),
                ("Шаг 4 — Появление 220 В", "220V_ON", "ЗАМКНИТЕ пин D5 на GND и держите (4+ сек)"),
            ]
        else:
            steps_seq = [
                ("Шаг 3 — Появление 220 В", "220V_ON", "ЗАМКНИТЕ пин D5 на GND и держите (4+ сек)"),
                ("Шаг 4 — Пропадание 220 В", "220V_OFF", "РАЗОМКНИТЕ пин D5 от GND (оставьте в воздухе на 4+ сек)"),
            ]

        for label, target, prompt in steps_seq:
            print(f"\n[{label}] {prompt}")
            ans = input("  Нажмите Enter когда сделаете (или 's' + Enter для пропуска): ").strip().lower()
            if ans == "s":
                self.record(label, "SKIP", "Пропущено пользователем")
                continue
            print("  Ожидаю событие от платы (антидребезг 3 сек + HTTPS отправка)...")
            seen, vlg, tg = self.wait_for_mains_change(target, timeout_sec=30.0)
            if not seen:
                self.record(label, "FAIL", f"Не получено событие {target} за 30 сек. Проверьте контакт D5 (GPIO14) и GND.")
            else:
                detail = f"220В переключено в {target}; VLG=[{vlg or 'нет ответа'}]; TG=[{tg or 'не настроен/нет'}]"
                if status.get("tg_configured") and (not tg or "FAIL" in tg):
                    self.record(label, "WARN", detail)
                else:
                    self.record(label, "PASS", detail)

        # STEP 5: Debounce noise immunity (< 2s)
        print("\n[Шаг 5/7] Проверка фильтра помех (антидребезг 3 сек).")
        ans = input("  Коротко коснитесь D5 <-> GND на 1 сек, уберите и сразу нажмите Enter ('s' — пропуск): ").strip().lower()
        if ans == "s":
            self.record("Шаг 5 — Фильтр помех (< 2 сек)", "SKIP", "Пропущено пользователем")
        else:
            print("  Наблюдаю 6 секунд, ложных срабатываний быть не должно...")
            false_events = self.check_no_false_trigger(watch_sec=6.0)
            if false_events:
                self.record("Шаг 5 — Фильтр помех (< 2 сек)", "FAIL", f"Было ложное срабатывание: {false_events}")
            else:
                self.record("Шаг 5 — Фильтр помех (< 2 сек)", "PASS", "0 ложных срабатываний за 6 сек")

        # STEP 6: Memory soak / repeated HTTPS sends
        print(f"\n[Шаг 6/7] Проверка утечки памяти и фрагментации ({stress_cycles} переключений D5)...")
        ans = input("  Нажмите Enter для старта серии переключений (или 's' + Enter для пропуска): ").strip().lower()
        if ans == "s" or stress_cycles <= 0:
            self.record("Шаг 6 — Стабильность RAM/BearSSL", "SKIP", "Пропущено пользователем")
        else:
            start_status = self.query_status()
            start_heap = start_status.get("heap_now", 0)
            cur_state = start_status.get("mains_220v", "OFF")
            completed = 0
            for i in range(1, stress_cycles + 1):
                next_target = "220V_OFF" if cur_state == "ON" else "220V_ON"
                action = "РАЗОМКНИТЕ D5 от GND" if next_target == "220V_OFF" else "ЗАМКНИТЕ D5 на GND"
                print(f"  Цикл {i}/{stress_cycles}: {action} и ждите 4 сек...")
                seen, _, _ = self.wait_for_mains_change(next_target, timeout_sec=30.0)
                if seen:
                    completed += 1
                    cur_state = "ON" if next_target == "220V_ON" else "OFF"
                else:
                    print(f"    [WARN] Таймаут ожидания {next_target} на цикле {i}")
                    break
            end_status = self.query_status()
            end_heap = end_status.get("heap_now", 0)
            end_block = end_status.get("block_now", 0)
            end_frag = end_status.get("frag_now", 100)
            drop = start_heap - end_heap
            m_stat, m_issues = evaluate_memory(end_heap, end_block, end_frag)
            if drop > 4000 and m_stat == "PASS":
                m_stat = "WARN"
                m_issues.append(f"просадка кучи на {drop} B")
            self.record(
                "Шаг 6 — Стабильность RAM/BearSSL",
                m_stat,
                f"циклов {completed}/{stress_cycles}; heap {start_heap}->{end_heap} B (delta={-drop:+d}), "
                f"maxBlk={end_block} B, frag={end_frag}%" + (f" ({'; '.join(m_issues)})" if m_issues else ""),
            )

        # STEP 7: Full DUMP
        print("\n[Шаг 7/7] Снятие полного дампа LittleFS (команда DUMP)...")
        dump_text = self.query_dump()
        if "----- END ESP8266 DIAG DUMP -----" in dump_text:
            self.record("Шаг 7 — Дамп LittleFS", "PASS", f"Получено {len(dump_text)} байт истории")
        else:
            self.record("Шаг 7 — Дамп LittleFS", "WARN", f"Дамп неполный ({len(dump_text)} байт)")

        return status, dump_text

    def save_report(self, dump_text: str) -> str:
        os.makedirs("logs", exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join("logs", f"esp_test_report_{ts}.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("=== ESP8266 AUTOMATED TEST REPORT ===\n")
            f.write(f"timestamp: {datetime.datetime.now().isoformat(timespec='seconds')}\n")
            f.write(f"port: {self.port} @ {self.baud}\n\n")
            f.write("--- SUMMARY ---\n")
            for name, st, details in self.results:
                f.write(f"[{st}] {name}: {details}\n")
            f.write("\n--- FULL LITTLEFS DUMP ---\n")
            f.write(dump_text or "(empty)\n")
            f.write("\n--- SESSION SERIAL TRANSCRIPT ---\n")
            f.write("\n".join(self.raw_transcript))
            f.write("\n")
        return os.path.abspath(path)


def run_self_test():
    sample_status = """=== Korotych ESP8266 Diagnostics ===
fw_version: 1.0.2
sensor_id: esp_a1b2c3
sensor_label: Test Bench
uptime_sec: 145
mains_220v: OFF
wifi_status: 3 (ip=192.168.1.42, rssi=-54 dBm, disc=0, last_disc_reason=0)
heap_now: 35800 B (min_seen=33100 B)
max_block_now: 31200 B (min_seen=27500 B)
frag_now: 7% (max_seen=12%)
reset_reason: Power On
reset_info: Fatal exception:0 flag:6 (EXT_SYS_RST) epc1:0x00000000
village: ok=4 fail=0 last_http=200 ssl_err=''
telegram: configured=yes ok=2 fail=0 last_http=200 ssl_err=''
endpoints: /status | /diag (download full log) | /clear
"""
    parsed = parse_status_block(sample_status)
    assert parsed["fw_version"] == "1.0.2", parsed
    assert parsed["sensor_id"] == "esp_a1b2c3", parsed
    assert parsed["wifi_status"] == 3 and parsed["ip"] == "192.168.1.42", parsed
    assert parsed["heap_now"] == 35800 and parsed["block_now"] == 31200 and parsed["frag_now"] == 7, parsed
    assert parsed["tg_configured"] is True and parsed["vlg_http"] == 200, parsed

    st_ok, iss_ok = evaluate_memory(35800, 31200, 7)
    assert st_ok == "PASS" and not iss_ok, (st_ok, iss_ok)

    st_fail, iss_fail = evaluate_memory(11500, 14000, 42)
    assert st_fail == "FAIL" and len(iss_fail) == 3, (st_fail, iss_fail)

    log_parsed = parse_diag_line("[145][MAINS] Change -> 220V_ON | h=35120 b=30400 f=8% rssi=-55")
    assert log_parsed and log_parsed["tag"] == "MAINS" and log_parsed["heap"] == 35120, log_parsed

    print("[SELF-TEST] PASS: status parser, diagLog parser, and memory evaluator verified.")


def main():
    parser = argparse.ArgumentParser(description="Interactive automated tester for ESP8266 light sensor.")
    parser.add_argument("--port", help="Serial port (e.g. COM3). Auto-detected if omitted.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument("--cycles", type=int, default=3, help="Number of D5 toggles for Step 6 stress test (default: 3)")
    parser.add_argument("--self-test", action="store_true", help="Run built-in unit self-test without hardware")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return

    port = select_serial_port(args.port)
    tester = EspTester(port, args.baud)
    dump_text = ""
    try:
        tester.open()
        out = tester.run_all(stress_cycles=args.cycles)
        if isinstance(out, tuple):
            _, dump_text = out
    except KeyboardInterrupt:
        print("\n[INFO] Тест прерван пользователем (Ctrl+C).")
    finally:
        tester.close()

    report_path = tester.save_report(dump_text)
    fails = sum(1 for _, st, _ in tester.results if st == "FAIL")
    warns = sum(1 for _, st, _ in tester.results if st == "WARN")

    print("\n============================================================")
    if fails == 0 and warns == 0:
        print("  ИТОГ: ВСЕ ТЕСТЫ ПРОЙДЕНЫ УСПЕШНО (PASS)")
    elif fails == 0:
        print(f"  ИТОГ: ЕСТЬ ПРЕДУПРЕЖДЕНИЯ (WARN={warns}, FAIL=0)")
    else:
        print(f"  ИТОГ: НАЙДЕНЫ ОШИБКИ (FAIL={fails}, WARN={warns})")
    print(f"  Файл отчёта: {report_path}")
    print("  Передайте этот .txt файл разработчику для разбора.")
    print("============================================================")


if __name__ == "__main__":
    main()
