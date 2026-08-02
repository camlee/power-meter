#!/usr/bin/env python3
"""Run read-only acceptance checks against a real power-meter device."""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request

from mdns_resolver import normalize_hostname, resolve_ipv4


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
LIVE_LAYOUTS = {
    0x344D5056: (4, 84),    # VPM4 replay frame
    0x354D5056: (5, 128),  # VPM5 current frame
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def resolve_host(value):
    parsed = urllib.parse.urlsplit(value if "://" in value else "//" + value)
    if not parsed.hostname:
        raise ValueError("invalid host: {}".format(value))
    host = normalize_hostname(parsed.hostname)
    addresses = resolve_ipv4(host)
    return host, addresses[0] if addresses else host, parsed.port


def fetch(address, port, path, timeout):
    url = "http://{}:{}{}".format(address, port, path)
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.headers, response.read()


def fetch_json(address, port, path, timeout):
    headers, body = fetch(address, port, path, timeout)
    content_type = headers.get_content_type()
    require(content_type == "application/json", "{} returned {}".format(path, content_type))
    try:
        return json.loads(body.decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("{} returned invalid JSON: {}".format(path, error))


def check_ui(address, port, timeout):
    headers, body = fetch(address, port, "/", timeout)
    require(headers.get_content_type() == "text/html", "root did not return HTML")
    require(len(body) > 0, "root returned an empty response")
    return "UI served ({} bytes)".format(len(body))


def check_status(address, port, timeout, expected_device, expected_profile):
    status = fetch_json(address, port, "/api/v1/web/status", timeout)
    require(status.get("api_version") == 1, "unsupported web status API version")
    require(isinstance(status.get("device_id"), str) and status["device_id"],
            "web status has no device identity")
    require(isinstance(status.get("hardware_profile"), str) and status["hardware_profile"],
            "web status has no hardware profile")
    require(int(status.get("ws_connection_limit", 0)) >= 1,
            "device reports no WebSocket capacity")
    if expected_device:
        require(status["device_id"] == expected_device,
                "expected device {}, got {}".format(expected_device, status["device_id"]))
    if expected_profile:
        require(status["hardware_profile"] == expected_profile,
                "expected profile {}, got {}".format(expected_profile, status["hardware_profile"]))
    return "{} running {} ({})".format(
        status["device_id"], status.get("build_version", "unknown build"),
        status["hardware_profile"])


def check_health(address, port, timeout):
    debug = fetch_json(address, port, "/api/v1/debug", timeout)
    require(debug.get("api_version") == 1, "unsupported debug API version")
    storage = debug.get("storage")
    require(isinstance(storage, dict) and storage.get("mounted") is True,
            "device storage is not mounted")
    ota = debug.get("ota")
    require(isinstance(ota, dict), "debug status has no OTA health")
    require(ota.get("health") == "confirmed",
            "running firmware is not confirmed (health={})".format(ota.get("health")))
    require(ota.get("rollback_detected") is False, "device reports an OTA rollback")
    return "storage mounted; OTA image confirmed"


def check_sensors(address, port, timeout):
    sensors = fetch_json(address, port, "/api/v1/sensors", timeout)
    require(sensors.get("api_version") == 1, "unsupported sensors API version")
    source = sensors.get("source")
    require(isinstance(source, dict) and isinstance(source.get("mode"), str),
            "sensor response has no source mode")
    channels = sensors.get("channels")
    require(isinstance(channels, list) and channels, "sensor response has no channels")
    required = {"id", "configured", "observed", "state", "voltage", "current", "power"}
    for index, channel in enumerate(channels):
        require(isinstance(channel, dict), "sensor channel {} is not an object".format(index))
        missing = sorted(required.difference(channel))
        require(not missing, "sensor channel {} is missing {}".format(index, ", ".join(missing)))
    require(any(channel["configured"] for channel in channels),
            "device has no configured sensor channels")
    return "{} sensor channel(s) in {} mode".format(len(channels), source["mode"])


def take(sock, buffered, count):
    while len(buffered) < count:
        chunk = sock.recv(max(4096, count - len(buffered)))
        if not chunk:
            raise RuntimeError("WebSocket closed before a live frame arrived")
        buffered.extend(chunk)
    result = bytes(buffered[:count])
    del buffered[:count]
    return result


def read_frame(sock, buffered):
    first, second = take(sock, buffered, 2)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", take(sock, buffered, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", take(sock, buffered, 8))[0]
    require(not masked, "server sent an invalid masked WebSocket frame")
    return opcode, take(sock, buffered, length)


def open_socket(host_header, address, port, timeout):
    sock = socket.create_connection((address, port), timeout=timeout)
    sock.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        "GET /api/v1/live HTTP/1.1\r\n"
        "Host: {}:{}\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: {}\r\n\r\n"
    ).format(host_header, port, key)
    sock.sendall(request.encode("ascii"))

    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed during the WebSocket handshake")
        response.extend(chunk)
        require(len(response) <= 8192, "WebSocket handshake response was too large")
    header_bytes, remaining = bytes(response).split(b"\r\n\r\n", 1)
    lines = header_bytes.decode("iso-8859-1").split("\r\n")
    require(lines[0].startswith("HTTP/1.1 101 "),
            "WebSocket upgrade failed: {}".format(lines[0]))
    headers = {}
    for line in lines[1:]:
        name, separator, value = line.partition(":")
        if separator:
            headers[name.lower()] = value.strip()
    expected = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")
    require(headers.get("sec-websocket-accept") == expected,
            "WebSocket handshake returned an invalid accept key")
    return sock, bytearray(remaining)


def verify_live_frame(payload):
    require(len(payload) >= 6,
            "live WebSocket frame was too short: {} bytes".format(len(payload)))
    magic = struct.unpack_from("<I", payload)[0]
    expected = LIVE_LAYOUTS.get(magic)
    require(expected is not None,
            "live WebSocket frame had unknown magic 0x{:08x}".format(magic))
    version, size = expected
    require(payload[4] == version and payload[5] == 1 and len(payload) == size,
            "invalid VPM{} live frame: version={}, type={}, size={}".format(
                version, payload[4], payload[5], len(payload)))
    return version, size


def check_live(host, address, http_port, websocket_port, timeout):
    sock = None
    try:
        sock, buffered = open_socket(host, address, websocket_port, timeout)
        while True:
            opcode, payload = read_frame(sock, buffered)
            if opcode == 2:
                version, size = verify_live_frame(payload)
                break
            if opcode == 1:
                raise RuntimeError("WebSocket returned text instead of live data")
            if opcode == 8:
                raise RuntimeError("WebSocket closed before publishing live data")
        status = fetch_json(address, http_port, "/api/v1/web/status", timeout)
        connections = int(status.get("ws_connections", 0))
        require(connections >= 1,
                "live data arrived but status reports zero WebSocket connections")
        return "VPM{} frame ({} bytes), {} connection(s)".format(
            version, size, connections)
    finally:
        if sock is not None:
            sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="meter hostname, mDNS name, IPv4 address, or HTTP URL")
    parser.add_argument("--http-port", type=int,
                        help="HTTP port (default: URL port or 80)")
    parser.add_argument("--websocket-port", type=int, default=81)
    parser.add_argument("--timeout", type=float, default=6)
    parser.add_argument("--expected-device", help="require this device_id")
    parser.add_argument("--expected-profile", help="require this hardware_profile")
    args = parser.parse_args()

    display_host = args.host
    active_check = "Connection"
    try:
        display_host, address, url_port = resolve_host(args.host)
        http_port = args.http_port or url_port or 80
        checks = (
            ("Web UI", lambda: check_ui(address, http_port, args.timeout)),
            ("Identity", lambda: check_status(
                address, http_port, args.timeout,
                args.expected_device, args.expected_profile)),
            ("Health", lambda: check_health(address, http_port, args.timeout)),
            ("Sensors", lambda: check_sensors(address, http_port, args.timeout)),
            ("Live WebSocket", lambda: check_live(
                display_host, address, http_port,
                args.websocket_port, args.timeout)),
        )
        for name, check in checks:
            active_check = name
            print("PASS {:<14} {}".format(name, check()), flush=True)
        print("Device acceptance passed for {} ({})".format(display_host, address))
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as error:
        sys.exit("FAIL {} for {}: {}".format(active_check, display_host, error))


if __name__ == "__main__":
    main()
