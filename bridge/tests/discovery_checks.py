#!/usr/bin/env python3
"""Isolated loopback only; no discovery traffic is sent onto a LAN."""
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("discovery", ROOT / "tools/discover.py")
discovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discovery)
NETWORK = ipaddress.IPv4Network("127.0.0.0/8")


class FakeReplies:
    def __init__(self, mode="ready"):
        self.mode = mode
        self.channel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.channel.bind(("127.0.0.1", 0))
        self.channel.settimeout(0.6)
        self.foreign = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.foreign.bind(("127.0.0.1", 0))
        self.port = self.channel.getsockname()[1]
        self.packets = []
        self.thread = threading.Thread(target=self.run)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.thread.join(1.5)
        self.channel.close()
        self.foreign.close()
        if self.thread.is_alive():
            raise AssertionError("fake peer exceeded its deadline")

    def run(self):
        try:
            while len(self.packets) < 8:
                data, peer = self.channel.recvfrom(1024)
                self.packets.append(data)
                fields = discovery.HEADER.unpack(data)
                if fields[2] == 5:
                    return
                if fields[2] != 1:
                    return
                boot = bytes([0x61 if self.mode != "changed" or len(self.packets) == 1 else 0x62]) * 16
                available = self.mode != "unavailable"
                response = discovery.HEADER.pack(b"SCX2", 2, 2, int(available), 0, fields[5], boot, bytes([0x71])*16 if available else bytes(16), 0)
                if self.mode == "foreign":
                    self.foreign.sendto(response, peer)
                elif self.mode == "malformed":
                    self.channel.sendto(response + b"extra", peer)
                    self.channel.sendto(response[:8] + bytes(16) + response[24:], peer)
                else:
                    self.channel.sendto(response, peer)
        except socket.timeout:
            pass


class DiscoveryChecks(unittest.TestCase):
    def test_wire_matches_cpp_golden_vectors_and_stop_has_no_state(self):
        vectors = json.loads((ROOT / "fixtures/scx2-wire.json").read_text())["vectors"]
        by_name = {v["name"]: v for v in vectors}
        session = bytes.fromhex(by_name["hello"]["session"])
        self.assertEqual(discovery.hello(session).hex(), by_name["hello"]["hex"])
        offer = discovery.ready(bytes.fromhex(by_name["ready"]["hex"]), session)
        expected = bytes.fromhex(by_name["stop-max"]["hex"][:-16] + "0000000000000001")
        self.assertEqual(discovery.stop(session, offer), expected)
        self.assertIsNone(discovery.stop(session, discovery.ready(bytes.fromhex(by_name["unavailable"]["hex"]), session)))

    def test_strict_ready_validation(self):
        session = b"s" * 16
        data = discovery.HEADER.pack(b"SCX2", 2, 2, 1, 0, session, b"b"*16, b"c"*16, 0)
        bad = [data[:-1], data+b"x", data[:4]+bytes([3])+data[5:], data[:5]+bytes([4])+data[6:],
               data[:6]+bytes([3])+data[7:], data[:7]+bytes([1])+data[8:], data[:8]+b"x"*16+data[24:],
               data[:24]+bytes(16)+data[40:], data[:40]+bytes(16)+data[56:], data[:-1]+b"\x01"]
        for packet in bad:
            self.assertIsNone(discovery.ready(packet, session))

    def test_unicast_confirmation_and_scoped_stop_only(self):
        with FakeReplies() as peer:
            results = discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", peer.port, 0.15)
        self.assertEqual(results, [{"ip":"127.0.0.1", "port":peer.port, "hid_ready":True}])
        self.assertEqual([p[5] for p in peer.packets], [1, 1, 5])
        self.assertEqual(peer.packets[0], peer.packets[1])
        self.assertTrue(all(len(p)==64 for p in peer.packets))

    def test_wrong_source_port_is_ignored_without_confirmation(self):
        with FakeReplies("foreign") as peer:
            self.assertEqual(discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", peer.port, 0.1), [])
        self.assertEqual([p[5] for p in peer.packets], [1])

    def test_malformed_and_other_session_are_ignored(self):
        with FakeReplies("malformed") as peer:
            self.assertEqual(discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", peer.port, 0.1), [])
        self.assertEqual([p[5] for p in peer.packets], [1])

    def test_changed_boot_does_not_confirm_and_original_offer_is_stopped(self):
        with FakeReplies("changed") as peer:
            self.assertEqual(discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", peer.port, 0.15), [])
        self.assertEqual([p[5] for p in peer.packets], [1, 1, 5])
        self.assertEqual(peer.packets[-1][24:40], b"a"*16)

    def test_usb_unavailable_is_reported_without_invalid_stop(self):
        with FakeReplies("unavailable") as peer:
            results = discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", peer.port, 0.1)
        self.assertFalse(results[0]["hid_ready"])
        self.assertEqual([p[5] for p in peer.packets], [1, 1])

    def test_nonlocal_targets_and_unbounded_time_refused_before_socket(self):
        with patch.object(discovery.socket, "socket", side_effect=AssertionError("must not open")):
            for target, timeout in [("192.0.2.1", 1), ("127.0.0.1", 6), ("127.0.0.1", float("nan"))]:
                self.assertRaises(discovery.DiscoveryError, discovery.discover, "127.0.0.1", NETWORK, target, 42736, timeout)
            self.assertRaises(discovery.DiscoveryError, discovery.main, ["--host","192.0.2.1"])

    def test_mac_interface_read_uses_only_local_metadata(self):
        replies = [" interface: en0\n", " inet 192.168.7.12 netmask 0xffffff00 broadcast 192.168.7.255\n"]
        with patch.object(discovery.subprocess,"check_output",side_effect=replies) as query:
            local, network, target = discovery.mac_network()
        self.assertEqual((local,str(network),target),("192.168.7.12","192.168.7.0/24","192.168.7.255"))
        self.assertEqual(query.call_count,2)

    def test_actual_cpp_receiver_never_arms_and_probe_offer_is_released(self):
        executable = os.environ.get("SC_DISCOVERY_PEER")
        self.assertIsNotNone(executable, "run with the actual compiled C++ loopback peer")
        peer = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        def line():
            self.assertTrue(select.select([peer.stdout],[],[],2)[0], "peer reply timeout")
            return peer.stdout.readline().strip()
        def command(text):
            peer.stdin.write(text+"\n");peer.stdin.flush();return line()
        try:
            port = int(line().split()[1])
            self.assertEqual(command("auto on"),"OK")
            self.assertEqual(discovery.discover("127.0.0.1", NETWORK, "127.0.0.1", port, 0.2), [{"ip":"127.0.0.1","port":port,"hid_ready":True}])
            status = command("status").split()
            self.assertEqual(status[:2],["0","0"])
            self.assertEqual(status[2],"3")  # HELLO, same-session HELLO, STOP; no STATE.
            self.assertTrue(all(value == "0" for value in command("reports").split(",") if value))
            self.assertEqual(command("quit"),"OK")
            self.assertEqual(peer.wait(timeout=2),0)
        finally:
            if peer.poll() is None:
                peer.kill();peer.wait(timeout=2)
            peer.stdin.close();peer.stdout.close();peer.stderr.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
