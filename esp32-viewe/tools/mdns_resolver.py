#!/usr/bin/env python3
"""Resolve local IPv4 mDNS names without third-party Python packages.

This is intentionally a small hostname resolver, not a general DNS-SD
implementation. It is used by the OTA uploader so `.local` updates do not
depend on NSS, Avahi, or the `zeroconf` Python package.
"""
from __future__ import annotations

import argparse
import json
import select
import socket
import struct
import sys
import time


MDNS_ADDRESS = "224.0.0.251"
MDNS_PORT = 5353


def normalize_hostname(hostname: str) -> str:
    hostname = hostname.strip().rstrip(".").lower()
    if "." not in hostname:
        hostname += ".local"
    return hostname


def _encode_name(hostname: str) -> bytes:
    encoded = bytearray()
    for label in normalize_hostname(hostname).split("."):
        part = label.encode("utf-8")
        if not part or len(part) > 63:
            raise ValueError("Invalid mDNS hostname: {}".format(hostname))
        encoded.append(len(part))
        encoded.extend(part)
    encoded.append(0)
    return bytes(encoded)


def _decode_name(packet: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    next_offset = None
    visited: set[int] = set()
    while True:
        if offset >= len(packet) or offset in visited:
            raise ValueError("Invalid compressed DNS name")
        visited.add(offset)
        length = packet[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                raise ValueError("Truncated DNS compression pointer")
            if next_offset is None:
                next_offset = offset + 2
            offset = ((length & 0x3F) << 8) | packet[offset + 1]
            continue
        if length & 0xC0 or offset + 1 + length > len(packet):
            raise ValueError("Invalid DNS label")
        labels.append(packet[offset + 1:offset + 1 + length].decode("utf-8"))
        offset += 1 + length
    return ".".join(labels).lower(), next_offset if next_offset is not None else offset


def _parse_a_records(packet: bytes, hostname: str) -> list[str]:
    if len(packet) < 12:
        return []
    _, flags, question_count, answer_count, authority_count, additional_count = \
        struct.unpack_from("!HHHHHH", packet)
    if not flags & 0x8000:
        return []
    offset = 12
    try:
        for _ in range(question_count):
            _, offset = _decode_name(packet, offset)
            offset += 4
            if offset > len(packet):
                return []
        addresses = []
        for _ in range(answer_count + authority_count + additional_count):
            name, offset = _decode_name(packet, offset)
            if offset + 10 > len(packet):
                return []
            record_type, record_class, ttl, length = struct.unpack_from("!HHIH", packet, offset)
            offset += 10
            if offset + length > len(packet):
                return []
            if (name == hostname and record_type == 1 and
                    record_class & 0x7FFF == 1 and ttl > 0 and length == 4):
                addresses.append(socket.inet_ntoa(packet[offset:offset + 4]))
            offset += length
        return addresses
    except (UnicodeDecodeError, ValueError, struct.error):
        return []


def _interface_ipv4_addresses() -> list[str]:
    addresses: set[str] = set()
    try:
        import fcntl

        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            for _, name in socket.if_nameindex():
                request = struct.pack("256s", name[:15].encode("utf-8"))
                try:
                    response = fcntl.ioctl(probe.fileno(), 0x8915, request)  # SIOCGIFADDR
                    address = socket.inet_ntoa(response[20:24])
                    if not address.startswith("127."):
                        addresses.add(address)
                except OSError:
                    continue
        finally:
            probe.close()
    except (ImportError, OSError):
        pass

    # Portable fallback for platforms without SIOCGIFADDR. Connecting a UDP
    # socket chooses the interface without sending a packet.
    if not addresses:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect((MDNS_ADDRESS, MDNS_PORT))
            address = probe.getsockname()[0]
            if address and not address.startswith("127."):
                addresses.add(address)
        except OSError:
            pass
        finally:
            probe.close()
    return sorted(addresses)


def resolve_ipv4(hostname: str, timeout: float = 1.5) -> list[str]:
    """Return IPv4 addresses advertised for hostname over local mDNS."""
    hostname = normalize_hostname(hostname)
    query = (struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + _encode_name(hostname) +
             struct.pack("!HH", 1, 1))  # A, IN
    interfaces = _interface_ipv4_addresses()
    if not interfaces:
        return []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", MDNS_PORT))
        for interface_address in interfaces:
            membership = socket.inet_aton(MDNS_ADDRESS) + socket.inet_aton(interface_address)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
        sock.setblocking(False)
    except OSError:
        sock.close()
        return []

    deadline = time.monotonic() + max(0.05, timeout)
    next_query = 0.0
    found: set[str] = set()
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_query:
                for interface_address in interfaces:
                    try:
                        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                                        socket.inet_aton(interface_address))
                        sock.sendto(query, (MDNS_ADDRESS, MDNS_PORT))
                    except OSError:
                        pass
                next_query = now + 0.4
            wait = min(deadline - now, max(0.0, next_query - now))
            readable, _, _ = select.select([sock], [], [], wait)
            if readable:
                try:
                    packet, _ = sock.recvfrom(9000)
                except OSError:
                    continue
                found.update(_parse_a_records(packet, hostname))
            if found:
                return sorted(found)
    finally:
        sock.close()
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hostname", nargs="+", help="hostname(s), with or without .local")
    parser.add_argument("--timeout", type=float, default=1.5, help="resolution timeout per name")
    parser.add_argument("--json", action="store_true", help="emit structured output")
    args = parser.parse_args()
    results = [
        {"hostname": normalize_hostname(hostname),
         "addresses": resolve_ipv4(hostname, args.timeout)}
        for hostname in args.hostname
    ]
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for result in results:
            addresses = ", ".join(result["addresses"]) or "not found"
            print("{}\t{}".format(result["hostname"], addresses))
    return 0 if all(result["addresses"] for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
