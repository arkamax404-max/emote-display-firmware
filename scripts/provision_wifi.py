#!/usr/bin/env python3
"""Provision over USB without storing credentials in the source or CLI arguments."""
import argparse
import getpass
import time

import serial


def wait_for(port, prefix, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("WIFI_ERROR"):
            raise RuntimeError(line)
        if line.startswith(prefix):
            return
    raise TimeoutError(f"No response: {prefix}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="e.g. /dev/cu.usbmodem14101")
    args = parser.parse_args()
    ssid = input("SSID (Wi-Fi de 2,4 GHz): ")
    password = getpass.getpass("Contraseña (vacía si la red es abierta): ")
    if not 1 <= len(ssid.encode("utf-8")) <= 32 or any(c in ssid for c in "\r\n\0"):
        parser.error("SSID must contain 1–32 bytes, without newline/NUL")
    if (password and not 8 <= len(password.encode("utf-8")) <= 63) or any(c in password for c in "\r\n\0"):
        parser.error("Password must contain 8–63 bytes, or be empty for an open network")
    with serial.Serial(args.port, 115200, timeout=0.25) as port:
        time.sleep(2)
        port.reset_input_buffer()
        port.write(b"WIFI\n")
        wait_for(port, "WIFI_SSID")
        port.write(ssid.encode("utf-8") + b"\n")
        wait_for(port, "WIFI_PASSWORD")
        port.write(password.encode("utf-8") + b"\n")
        wait_for(port, "WIFI_SAVED")
    print("Credenciales guardadas en la placa. Reiniciando; consulta la IP en el monitor serie.")


if __name__ == "__main__":
    main()
