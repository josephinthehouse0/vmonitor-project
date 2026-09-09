#!/usr/bin/env python3
"""
test_negative.py - Gun 5: olumsuz senaryo testleri

monitor_server'in calisir durumda oldugu varsayilir (127.0.0.1:5000).
Her test PASS/FAIL olarak raporlanir; sonunda ozet basilir.

Kullanim:
    sudo ./monitor_server &   # (veya ayri bir terminalde on planda)
    python3 test_negative.py
"""
import socket
import time
import sys
import struct

HOST = "127.0.0.1"
PORT = 5000

results = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    results.append((name, status, detail))
    print(f"[{status}] {name}" + (f"  -- {detail}" if detail else ""))


def connect():
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.settimeout(3)
    return s


def send_line(s, line):
    s.sendall((line + "\n").encode())


def recv_line(s):
    """Reads until a newline or timeout; returns the decoded line (no \\n)."""
    buf = b""
    try:
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    line = buf.split(b"\n", 1)[0]
    return line.decode(errors="replace")


# ---------------------------------------------------------------------
# 1) Cok uzun satir (rx buffer'i asan, hic \n icermeyen veri)
# ---------------------------------------------------------------------
def test_oversized_line():
    try:
        s = connect()
        junk = b"A" * 5000
        s.sendall(junk)
        time.sleep(0.3)
        try:
            data = s.recv(1024)
            closed = (data == b"")
        except (ConnectionResetError, socket.timeout):
            closed = True
        check("Asiri uzun satir -> istemci baglantisi kesiliyor", closed)
        s.close()
    except Exception as e:
        check("Asiri uzun satir -> istemci baglantisi kesiliyor", False, f"exception: {e}")


def test_server_survives_oversized_line():
    try:
        s = connect()
        send_line(s, "1 GET")
        resp = recv_line(s)
        check("Sunucu, asiri uzun satir sonrasi hala cevap veriyor",
              resp.startswith("RSP 1 OK"), f"gelen: {resp!r}")
        s.close()
    except Exception as e:
        check("Sunucu, asiri uzun satir sonrasi hala cevap veriyor", False, f"exception: {e}")


def test_invalid_params():
    cases = [
        ("1 PERIOD abc", "PERIOD harf argumaniyla ERR donuyor"),
        ("2 PERIOD", "PERIOD argumansiz ERR donuyor"),
        ("3 PERIOD -5", "PERIOD negatif deger ile ERR donuyor"),
        ("4 PERIOD 0", "PERIOD sifir deger ile ERR donuyor"),
        ("5 THRESHOLD", "THRESHOLD argumansiz ERR donuyor"),
        ("6 THRESHOLD xyz", "THRESHOLD harf argumaniyla ERR donuyor"),
        ("7 INJECT", "INJECT argumansiz ERR donuyor"),
        ("8 INJECT notanumber", "INJECT harf argumaniyla ERR donuyor"),
        ("9 WATCH 2", "WATCH gecersiz deger (2) ile ERR donuyor"),
        ("10 WATCH", "WATCH argumansiz ERR donuyor"),
        ("11 FOOBAR", "Bilinmeyen komut ERR donuyor"),
    ]
    try:
        s = connect()
        for line, desc in cases:
            send_line(s, line)
            resp = recv_line(s)
            check(desc, "ERR" in resp, f"gonderilen: {line!r} gelen: {resp!r}")
        s.close()
    except Exception as e:
        check("Gecersiz parametre testleri", False, f"exception: {e}")


def test_malformed_lines():
    cases = [
        ("", "Bos satir -> malformed ERR"),
        ("5", "Sadece id, komut yok -> malformed ERR"),
        ("   ", "Sadece bosluk -> malformed ERR"),
    ]
    try:
        s = connect()
        for line, desc in cases:
            send_line(s, line)
            resp = recv_line(s)
            check(desc, "ERR" in resp, f"gonderilen: {line!r} gelen: {resp!r}")
        s.close()
    except Exception as e:
        check("Bozuk satir testleri", False, f"exception: {e}")


def test_abrupt_disconnect():
    try:
        s = connect()
        s.sendall(b"1 INJ")
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        s.close()
        time.sleep(0.3)

        s2 = connect()
        send_line(s2, "1 GET")
        resp = recv_line(s2)
        check("Ani (RST) baglanti kopmasi sonrasi sunucu ayakta kaliyor",
              resp.startswith("RSP 1 OK"), f"gelen: {resp!r}")
        s2.close()
    except Exception as e:
        check("Ani (RST) baglanti kopmasi sonrasi sunucu ayakta kaliyor",
              False, f"exception: {e}")


def test_multi_client_isolation():
    try:
        good = connect()
        bad = connect()

        bad.sendall(b"X" * 5000)

        send_line(good, "1 GET")
        resp = recv_line(good)
        check("Bir istemcinin kotu davranisi digerini etkilemiyor",
              resp.startswith("RSP 1 OK"), f"gelen: {resp!r}")

        good.close()
        try:
            bad.close()
        except Exception:
            pass
    except Exception as e:
        check("Bir istemcinin kotu davranisi digerini etkilemiyor",
              False, f"exception: {e}")


def test_connect_disconnect_churn():
    try:
        for i in range(20):
            s = connect()
            s.close()
        s = connect()
        send_line(s, "1 GET")
        resp = recv_line(s)
        check("20 hizli baglan/kopma sonrasi sunucu ayakta",
              resp.startswith("RSP 1 OK"), f"gelen: {resp!r}")
        s.close()
    except Exception as e:
        check("20 hizli baglan/kopma sonrasi sunucu ayakta", False, f"exception: {e}")


def main():
    print(f"=== Gun 5 Olumsuz Test Senaryolari (hedef: {HOST}:{PORT}) ===\n")

    test_oversized_line()
    test_server_survives_oversized_line()
    test_invalid_params()
    test_malformed_lines()
    test_abrupt_disconnect()
    test_multi_client_isolation()
    test_connect_disconnect_churn()

    print("\n=== OZET ===")
    passed = sum(1 for _, s, _ in results if s == "PASS")
    failed = sum(1 for _, s, _ in results if s == "FAIL")
    print(f"Toplam: {len(results)}  PASS: {passed}  FAIL: {failed}")

    if failed > 0:
        print("\nBasarisiz testler:")
        for name, status, detail in results:
            if status == "FAIL":
                print(f"  - {name}  ({detail})")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()