#!/usr/bin/env python3
"""Find a local SCX2 bridge without sending or arming any controller STATE.

One local broadcast HELLO, unicast confirmation, then a best-effort STOP release.
The session may reserve an idle receiver for up to its one-second offer lease.
This is discovery, not authentication or proof of USB/Xbox output.
"""
import argparse
import ipaddress
import json
import re
import select
import socket
import struct
import subprocess
import sys
import time
import uuid

PORT = 42736
HEADER = struct.Struct("!4sBBBB16s16s16sQ")
ZERO = bytes(16)
MAX_CANDIDATES = 16
MAX_DATAGRAMS = 256


class DiscoveryError(Exception):
    pass


def hello(session):
    return HEADER.pack(b"SCX2", 2, 1, 0, 0, session, ZERO, ZERO, 0)


def ready(data, session):
    if len(data) != HEADER.size:
        return None
    magic, version, kind, flags, reserved, actual_session, boot, challenge, sequence = HEADER.unpack(data)
    if (magic != b"SCX2" or version != 2 or kind != 2 or flags not in (0, 1)
            or reserved or actual_session != session or sequence or boot == ZERO):
        return None
    if (flags == 1) != (challenge != ZERO):
        return None
    return boot, challenge, flags == 1


def stop(session, offer):
    boot, challenge, hid_ready = offer
    if not hid_ready:
        return None
    return HEADER.pack(b"SCX2", 2, 5, 0, 0, session, boot, challenge, 1)


def mac_network(interface=None):
    """Read only the local interface/address; never query network passwords."""
    try:
        if interface is None:
            route = subprocess.check_output(["/sbin/route", "-n", "get", "default"], stderr=subprocess.DEVNULL, text=True)
            match = re.search(r"^\s*interface:\s*(\w+)\s*$", route, re.MULTILINE)
            if not match:
                raise DiscoveryError("No default local interface; connect the Mac to the same network as the Pico.")
            interface = match.group(1)
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9]{0,15}", interface):
            raise DiscoveryError("Invalid interface name.")
        details = subprocess.check_output(["/sbin/ifconfig", interface], stderr=subprocess.DEVNULL, text=True)
    except (OSError, subprocess.CalledProcessError):
        raise DiscoveryError("Could not inspect the Mac network interface.") from None
    match = re.search(r"\binet\s+(\d+\.\d+\.\d+\.\d+)\s+netmask\s+(0x[0-9a-fA-F]+)\s+broadcast\s+(\d+\.\d+\.\d+\.\d+)", details)
    if not match:
        raise DiscoveryError("The chosen interface has no IPv4 broadcast network; select the Wi-Fi/Ethernet interface with --interface.")
    local = ipaddress.IPv4Address(match.group(1))
    mask = ipaddress.IPv4Address(int(match.group(2), 16))
    network = ipaddress.IPv4Network(str(local) + "/" + str(mask), strict=False)
    if network.prefixlen < 8 or network.prefixlen > 30 or str(network.broadcast_address) != match.group(3):
        raise DiscoveryError("The interface does not have a supported bounded local broadcast network.")
    return str(local), network, str(network.broadcast_address)


def discover(local, network, destination, port=PORT, timeout=2.0):
    if not 1 <= port <= 65535 or not 0.05 <= timeout <= 5:
        raise DiscoveryError("Port or timeout is outside the supported bounds.")
    target = ipaddress.IPv4Address(destination)
    if target not in network or target.is_multicast or target.is_unspecified:
        raise DiscoveryError("Discovery must stay within the selected local IPv4 network.")
    session = uuid.uuid4().bytes
    probe = hello(session)
    candidates = {}
    found = []
    received = 0
    deadline = time.monotonic() + timeout
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as channel:
        channel.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        channel.bind((local, 0))
        channel.setblocking(False)
        channel.sendto(probe, (str(target), port))  # Exactly one broadcast or explicit loopback probe.

        def release(endpoint, candidate):
            if candidate["stopped"]:
                return
            packet = stop(session, candidate["offer"])
            if packet is not None:
                try:
                    channel.sendto(packet, endpoint)
                except OSError:
                    pass  # Offer expiry remains the independent fallback.
            candidate["stopped"] = True

        try:
            while received < MAX_DATAGRAMS:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([channel], [], [], remaining)[0]:
                    break
                try:
                    data, endpoint = channel.recvfrom(1024)
                except BlockingIOError:
                    continue
                received += 1
                address = ipaddress.IPv4Address(endpoint[0])
                if (endpoint[1] != port or address not in network or address.is_multicast
                        or address.is_unspecified or (network.prefixlen < 31 and address in (network.network_address, network.broadcast_address))):
                    continue
                offer = ready(data, session)
                if offer is None:
                    continue
                candidate = candidates.get(endpoint)
                if candidate is None:
                    if len(candidates) >= MAX_CANDIDATES:
                        continue
                    candidate = {"offer": offer, "stopped": False, "confirmed": False}
                    candidates[endpoint] = candidate
                    channel.sendto(probe, endpoint)  # Same-session unicast validation; never a STATE.
                elif not candidate["confirmed"] and candidate["offer"] == offer:
                    candidate["confirmed"] = True
                    found.append({"ip": endpoint[0], "port": port, "hid_ready": offer[2]})
                    release(endpoint, candidate)
        finally:
            for endpoint, candidate in candidates.items():
                release(endpoint, candidate)
    return sorted(found, key=lambda entry: entry["ip"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", help="Mac interface; otherwise use the default route")
    parser.add_argument("--host", help="Loopback only: explicit destination for isolated tests")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    if args.host:
        address = ipaddress.IPv4Address(args.host)
        if not address.is_loopback or args.interface:
            raise DiscoveryError("--host is only for isolated loopback tests; normal discovery uses the local interface.")
        local, network, destination = "127.0.0.1", ipaddress.IPv4Network("127.0.0.0/8"), str(address)
    else:
        if args.port != PORT:
            raise DiscoveryError("Network discovery uses only the bridge's fixed SCX2 port 42736.")
        local, network, destination = mac_network(args.interface)
    results = discover(local, network, destination, args.port, args.timeout)
    if args.json:
        print(json.dumps({"bridges": results, "controller_states_sent": 0}))
    elif results:
        for item in results:
            print("SCX2 bridge: " + item["ip"] + ":" + str(item["port"]) + (" (USB ready)" if item["hid_ready"] else " (USB not ready)"))
        print("No controller states sent. Open the app only after this probe finishes. This is not Xbox-output proof.")
    else:
        print("No idle SCX2 bridge replied. Check same-network Wi-Fi, board USB power, and disconnect the app before retrying. Some networks block broadcast.")
    return 0 if results else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (DiscoveryError, OSError, ValueError) as error:
        print("Discovery stopped: " + str(error), file=sys.stderr)
        sys.exit(2)
