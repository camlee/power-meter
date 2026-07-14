#!/usr/bin/env python3
"""Print VIEWE meter HTTP endpoints discovered through local mDNS.

No third-party Python package is required. The firmware advertises
_viewe-ota._tcp on port 80, so this is deliberately a bounded LAN discovery
method rather than a subnet scan. Pass --host when a serial monitor supplied
an IP address or mDNS is unavailable.
"""
from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys


def add(found: dict[str, dict[str, str]], host: str, address: str, port: str = "80") -> None:
    if address:
        url = f"http://{address}/" if port == "80" else f"http://{address}:{port}/"
        found[address] = {"host": host.rstrip("."), "address": address, "url": url}


def discover_avahi(found: dict[str, dict[str, str]]) -> None:
    command = shutil.which("avahi-browse")
    if not command:
        return
    try:
        output = subprocess.run([command, "-prt", "_viewe-ota._tcp"], check=False, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return
    for line in output.splitlines():
        if line.startswith("="):
            fields = line.split(";")
            # =;iface;proto;name;type;domain;host;address;port;txt
            if len(fields) >= 9:
                add(found, fields[6], fields[7], fields[8])


def discover_hostname(found: dict[str, dict[str, str]], hostname: str) -> None:
    try:
        addresses = socket.getaddrinfo(hostname, 80, type=socket.SOCK_STREAM)
    except socket.gaierror:
        return
    for _, _, _, _, sockaddr in addresses:
        add(found, hostname, sockaddr[0])


def main() -> int:
    parser = argparse.ArgumentParser(description="Discover VIEWE power meters on the local network")
    parser.add_argument("--host", help="known IP or hostname from serial; does not scan the network")
    parser.add_argument("--hostname", default="meter1.local", help="mDNS fallback name (default: meter1.local)")
    parser.add_argument("--json", action="store_true", help="emit structured output for scripts")
    args = parser.parse_args()
    found: dict[str, dict[str, str]] = {}
    if args.host:
        discover_hostname(found, args.host)
        if not found:
            add(found, args.host, args.host)
    else:
        discover_avahi(found)
        discover_hostname(found, args.hostname)
    values = list(found.values())
    if args.json:
        print(json.dumps(values, indent=2))
    else:
        for item in values:
            print(f"{item['url']}  ({item['host']})")
    return 0 if values else 1


if __name__ == "__main__":
    sys.exit(main())
