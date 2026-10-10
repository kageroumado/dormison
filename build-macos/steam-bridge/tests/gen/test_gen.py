#!/usr/bin/env python3
"""Generator fixtures: runs gen/gen_bridge.py on the fake SDK in tests/gen/sdk and checks
the marshaling rules it chose, then compiles both generated halves and drives the
Windows handlers against a fake object (handlers_test.cpp).

    python test_gen.py [--work DIR]

Needs the generator's environment (libclang's Python binding, Xcode's clang, mingw-w64);
`make test-gen` runs it with the venv's interpreter. DIR (default: a temporary
directory) receives the generated code and the test binaries.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(BRIDGE, "gen", "gen_bridge.py")
SDK = os.path.join(HERE, "sdk")
SOCKETS = "ISteamNetworkingSockets_SteamNetworkingSocketsFixture001"
STATS = "ISteamUserStats_SteamUserStatsFixture001"
WORK = None


def generate(out, only=None):
    args = [sys.executable, GEN, "--sdk-root", SDK, "--out", out, "--sdk-include", SDK, "--jobs", "3"]
    if only:
        args += ["--only", only]
    return subprocess.run(args, capture_output=True, text=True)


def read(*parts):
    with open(os.path.join(*parts)) as f:
        return f.read()


def handler(source, name):
    """The body of generated handler h_<name>."""
    m = re.search(rf"^bool h_{name}\(.*?^}}$", source, re.S | re.M)
    if not m:
        raise AssertionError(f"no handler h_{name}")
    return m.group(0)


class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = os.path.join(WORK, "generated")
        result = generate(cls.out, "SteamNetworkingSocketsFixture001,SteamUserStatsFixture001")
        if result.returncode != 0:
            raise AssertionError("generator failed:\n" + result.stderr)
        cls.report = read(cls.out, "REPORT.md")
        cls.win_sockets = read(cls.out, "win", f"{SOCKETS}.cpp")
        cls.mac_sockets = read(cls.out, "mac", f"{SOCKETS}.cpp")
        cls.win_stats = read(cls.out, "win", f"{STATS}.cpp")

    def test_count_before_array(self):
        self.assertIn("`CreateListenSocketIP` `pOptions` (const SteamNetworkingConfigValue_t *): elements = nOptions, in",
                      self.report)
        self.assertIn("`SetThings` `pThings` (const int32 *): elements = nThings, in", self.report)
        body = handler(self.win_sockets, "CreateListenSocketIP")
        self.assertIn("bridge::limit(a_nOptions, bridge::capacity_of(a_pOptions), 16);", body)
        self.assertIn("bridge::limit(a_nThings, bridge::capacity_of(a_pThings), 4);",
                      handler(self.win_sockets, "SetThings"))
        self.assertIn("c.put_in(pOptions, bridge::count(nOptions), 16, 16", self.mac_sockets)

    def test_const_array_without_rule(self):
        self.assertIn("- `PutValues`: pValue: no size rule while nCount could count it", self.report)
        self.assertNotIn("h_PutValues", self.win_sockets)
        self.assertIn('bridge::note_unsupported("ISteamNetworkingSockets_SteamNetworkingSocketsFixture001_PutValues")',
                      self.mac_sockets)

    def test_union_with_pointer(self):
        self.assertIn("- `SetVariant`: pVariant: FixtureVariant_t holds pointers", self.report)
        self.assertIn("- `SetVariantByValue`: variant: FixtureVariant_t holds pointers", self.report)
        self.assertIn("- `GetVariant`: returns FixtureVariant_t", self.report)

    def test_tagged_union_checked_on_both_sides(self):
        self.assertIn("tag `m_eDataType` ∈ {Int32, Int64, Float}", self.report)
        self.assertIn("if (!bridge::tags_allowed(pOptions, bridge::count(nOptions), 16, 4, 0xeu)) {", self.mac_sockets)
        body = handler(self.win_sockets, "CreateListenSocketIP")
        check = body.index("bridge::tags_allowed(a_pOptions.data, a_pOptions.size / 16, 16, 4, 0xeu)")
        self.assertLess(body.index("if (!q.done()) return false;"), check)
        self.assertLess(check, body.index("bridge::slot("))

    def test_record_returns(self):
        owner = handler(self.win_stats, "GetOwner")
        self.assertIn("typedef void * (*Fn)(void *, void *);", owner)
        self.assertIn("bridge::Blob ret = bridge::Blob::zeroed(8);", owner)
        totals = handler(self.win_stats, "GetTotals")
        self.assertIn("bridge::Blob ret = bridge::Blob::zeroed(16);", totals)
        self.assertIn("(obj, ret.data)", totals)

    def test_overloaded_virtuals(self):
        slots = {name: int(re.search(r"bridge::slot\(obj, (\d+)\)", handler(self.win_stats, name)).group(1))
                 for name in ("GetStat", "GetStat_2", "SetStat", "SetStat_2", "GetOwner")}
        self.assertEqual(slots, {"GetStat_2": 0, "GetStat": 1, "SetStat_2": 2, "SetStat": 3, "GetOwner": 4})

    def test_interfaces_issue_handles(self):
        body = handler(self.win_stats, "GetInterface")
        self.assertIn("p.w.u64(bridge::issue_handle(ret, a_pchVersion));", body)
        self.assertNotIn("uintptr_t", self.win_stats)

    def test_handlers_decode_before_calling(self):
        for source in (self.win_sockets, self.win_stats):
            for m in re.finditer(r"^bool h_\w+\(.*?^}$", source, re.S | re.M):
                body = m.group(0)
                if "bridge::slot(" not in body:
                    continue
                self.assertLess(body.index("if (!q.done()) return false;"), body.index("bridge::slot("), body)

    def test_both_halves_compile(self):
        if not shutil.which("x86_64-w64-mingw32-g++"):
            self.skipTest("mingw-w64 is not installed")
        mac_sources = [os.path.join(self.out, "mac", f) for f in read(self.out, "mac", "sources.txt").split()]
        dylib = os.path.join(WORK, "steamclient.dylib")
        transport = os.path.join(WORK, "transport.o")
        subprocess.run(["clang", "-c", "-w", "-o", transport, os.path.join(BRIDGE, "mac", "transport.c")], check=True)
        subprocess.run(["clang++", "-std=c++17", "-fno-rtti", "-dynamiclib", "-w",
                        "-I", os.path.join(BRIDGE, "mac"), "-I", os.path.join(BRIDGE, "shared"),
                        "-o", dylib, os.path.join(BRIDGE, "mac", "bridge.cpp"), transport, *mac_sources], check=True)
        win_sources = [os.path.join(self.out, "win", f) for f in read(self.out, "win", "sources.txt").split()]
        subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-static", "-w",
                        "-I", os.path.join(BRIDGE, "win"), "-I", os.path.join(BRIDGE, "shared"),
                        "-o", os.path.join(WORK, "sevo-steambridge.exe"),
                        os.path.join(BRIDGE, "win", "sevo_steambridge.cpp"), *win_sources, "-lws2_32"], check=True)

    def test_handlers_against_fake_object(self):
        win_sources = [os.path.join(self.out, "win", f) for f in read(self.out, "win", "sources.txt").split()]
        binary = os.path.join(WORK, "handlers_test")
        subprocess.run(["clang++", "-std=c++17", "-w", "-I", os.path.join(BRIDGE, "win"),
                        "-I", os.path.join(BRIDGE, "shared"), "-o", binary,
                        os.path.join(HERE, "handlers_test.cpp"), *win_sources], check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class Audit(unittest.TestCase):
    def test_plural_pointer_without_count_stops_generation(self):
        result = generate(os.path.join(WORK, "generated-audit"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ISteamApps_ReadHandles pHandles (const int32 *)", result.stderr)


def main():
    global WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=None)
    args, rest = ap.parse_known_args()
    WORK = args.work or tempfile.mkdtemp(prefix="bridge-gen-test-")
    os.makedirs(WORK, exist_ok=True)
    unittest.main(argv=[sys.argv[0], "-v", *rest])


if __name__ == "__main__":
    main()
