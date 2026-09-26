"""
Unit tests for msgbus.

Run:
    gcc -std=c++17 -Wall -Wextra -O2 -o msgbus msgbus.cpp
    python3 -m unittest test_msgbus.py -v

The tests exercise functional behaviour, boundary conditions, file
integrity, cleaning, command-line handling, and concurrency.  They
assume the constants in msgbus.cpp are:

    MAX_MESSAGES    10
    MAX_MSG_SIZE    100
    TTL_SECONDS     1

Change them here too if you change them there.
"""

import os
import struct
import subprocess
import tempfile
import time
import unittest


# ------------------------------------------------------------------ setup --

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.join(HERE, "msgbus")

# Mirror the on-disk layout from msgbus.cpp
HEADER_SIZE  = 64
DESC_SIZE    = 48
MAX_MESSAGES = 10
MAX_MSG_SIZE = 100
TTL_SECONDS  = 1
DATA_START   = HEADER_SIZE + MAX_MESSAGES * DESC_SIZE   # 544


class MsgbusBase(unittest.TestCase):
    """Shared setup: fresh temp dir, compiled binary, initialized bus."""

    def setUp(self):
        super().setUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.bus_path = os.path.join(self.tmp.name, "msgbus.db")
        self.bus(1, "init")

    # -- helpers -----------------------------------------------------------

    def bus(self, terminal_id, *args, expect_ok=True, timeout=15):
        """Run the binary synchronously and return the CompletedProcess."""
        cmd = [BINARY, str(terminal_id), *[str(a) for a in args]]
        try:
            r = subprocess.run(
                cmd, cwd=self.tmp.name,
                capture_output=True, text=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            self.fail(f"command timed out: {cmd}")
        if expect_ok:
            self.assertEqual(
                r.returncode, 0,
                msg=f"command failed: {cmd}\n"
                    f"stdout: {r.stdout!r}\n"
                    f"stderr: {r.stderr!r}"
            )
        return r

    def spawn(self, terminal_id, *args):
        """Start a process without waiting — for concurrency tests."""
        return subprocess.Popen(
            [BINARY, str(terminal_id), *[str(a) for a in args]],
            cwd=self.tmp.name,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def wait_all(self, procs, timeout=15):
        """Wait for all spawned processes, return list of (rc, out, err)."""
        results = []
        for p in procs:
            try:
                out, err = p.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                self.fail(f"child timed out: rc={p.returncode} err={err!r}")
            results.append((p.returncode, out, err))
        return results

    def read_raw(self):
        with open(self.bus_path, "rb") as f:
            return f.read()

    def write_raw(self, data):
        with open(self.bus_path, "r+b") as f:
            f.write(data)

    def desc_offset(self, slot):
        return HEADER_SIZE + slot * DESC_SIZE

    def get_descriptor(self, slot):
        data = self.read_raw()
        off = self.desc_offset(slot)
        return bytearray(data[off:off + DESC_SIZE])

    def put_descriptor_raw(self, slot, desc_bytes):
        """Write a descriptor as-is (checksum must already be correct)."""
        assert len(desc_bytes) == DESC_SIZE
        with open(self.bus_path, "r+b") as f:
            f.seek(self.desc_offset(slot))
            f.write(desc_bytes)

    def put_descriptor_corrupt(self, slot, desc_bytes):
        """Write a descriptor without touching its checksum."""
        self.put_descriptor_raw(slot, desc_bytes)


# ===================================================== basic functionality ==

class TestBasic(MsgbusBase):

    def test_init_creates_file(self):
        self.assertTrue(os.path.exists(self.bus_path))

    def test_init_file_size(self):
        self.assertEqual(os.path.getsize(self.bus_path), DATA_START)

    def test_send_and_read(self):
        self.bus(1, "send", 2, "hello")
        out = self.bus(2, "read").stdout
        self.assertIn("hello", out)
        self.assertIn("from terminal 1", out)

    def test_message_marked_processed(self):
        self.bus(1, "send", 2, "hello")
        self.bus(2, "read")
        out = self.bus(2, "read").stdout
        self.assertIn("no new messages", out)

    def test_receiver_isolation(self):
        self.bus(1, "send", 2, "for two")
        out = self.bus(3, "read").stdout
        self.assertNotIn("for two", out)
        self.assertIn("no new messages", out)

    def test_multiple_senders(self):
        self.bus(1, "send", 2, "AAA")
        self.bus(3, "send", 2, "BBB")
        out = self.bus(2, "read").stdout
        self.assertIn("AAA", out)
        self.assertIn("BBB", out)
        self.assertIn("from terminal 1", out)
        self.assertIn("from terminal 3", out)

    def test_message_with_spaces_and_specials(self):
        msg = 'hello, world! 123 $ "quoted" \\backslash'
        self.bus(1, "send", 2, msg)
        out = self.bus(2, "read").stdout
        self.assertIn(msg, out)

    def test_send_to_unknown_terminal(self):
        self.bus(1, "send", 99, "for the future")
        out = self.bus(99, "read").stdout
        self.assertIn("for the future", out)


# =============================================== boundary and validation ==

class TestBoundaries(MsgbusBase):

    def test_empty_message_rejected(self):
        r = self.bus(1, "send", 2, "", expect_ok=False)
        self.assertNotEqual(r.returncode, 0)
        # bus still usable
        self.bus(1, "send", 2, "ok")
        self.assertIn("ok", self.bus(2, "read").stdout)

    def test_message_exactly_max_size(self):
        msg = "x" * MAX_MSG_SIZE
        self.bus(1, "send", 2, msg)
        out = self.bus(2, "read").stdout
        self.assertIn(msg, out)

    def test_message_over_max_size_rejected(self):
        msg = "x" * (MAX_MSG_SIZE + 1)
        r = self.bus(1, "send", 2, msg, expect_ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("size", r.stderr.lower())

    def test_fill_to_max_messages(self):
        for i in range(MAX_MESSAGES):
            self.bus(1, "send", 99, f"UNIQUE{i}_END")
        out = self.bus(99, "read").stdout
        for i in range(MAX_MESSAGES):
            self.assertIn(f"UNIQUE{i}_END", out)

    def test_overflow_after_max_messages(self):
        for i in range(MAX_MESSAGES):
            self.bus(1, "send", 99, f"m{i}")
        r = self.bus(1, "send", 99, "one too many", expect_ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("full", r.stderr.lower())

    def test_clean_restores_capacity(self):
        # Fill up, then process and clean
        for i in range(MAX_MESSAGES):
            self.bus(1, "send", 99, f"m{i}")
        self.bus(99, "read")            # marks all processed
        self.bus(1, "clean")
        # Capacity restored
        self.bus(1, "send", 99, "after clean")
        out = self.bus(99, "read").stdout
        self.assertIn("after clean", out)


# ======================================================== cleaning / TTL ==

class TestCleaning(MsgbusBase):

    def test_clean_drops_processed_messages(self):
        self.bus(1, "send", 2, "AAA")
        self.bus(1, "send", 2, "BBB")
        self.bus(2, "read")             # both processed
        r = self.bus(1, "clean")
        self.assertIn("clean done", r.stdout)
        self.assertIn("0 message", r.stdout)

    def test_clean_keeps_unread_messages(self):
        self.bus(1, "send", 2, "keep me")
        self.bus(1, "clean")
        out = self.bus(2, "read").stdout
        self.assertIn("keep me", out)

    def test_clean_drops_expired_messages(self):
        self.bus(1, "send", 2, "OLD_MSG")
        time.sleep(TTL_SECONDS + 1)
        self.bus(1, "send", 2, "FRESH_MSG")
        self.bus(1, "clean")
        out = self.bus(2, "read").stdout
        self.assertIn("FRESH_MSG", out)
        self.assertNotIn("OLD_MSG", out)

    def test_clean_truncates_file(self):
        self.bus(1, "send", 2, "abc")
        self.bus(2, "read")
        size_before = os.path.getsize(self.bus_path)
        self.bus(1, "clean")
        size_after = os.path.getsize(self.bus_path)
        self.assertLess(size_after, size_before)
        self.assertEqual(size_after, DATA_START)


# =================================================== integrity / checksums ==

class TestIntegrity(MsgbusBase):

    def test_corrupted_descriptor_is_skipped(self):
        """A single flipped byte in a descriptor: reader skips that slot."""
        self.bus(1, "send", 2, "GOOD1")
        self.bus(1, "send", 2, "CORRUPTED")
        self.bus(1, "send", 2, "GOOD2")
        # Corrupt slot 1 without fixing the checksum
        d = self.get_descriptor(1)
        d[24] ^= 0xFF               # transmitter_id low byte
        self.put_descriptor_corrupt(1, bytes(d))
        out = self.bus(2, "read").stdout
        self.assertIn("GOOD1", out)
        self.assertIn("GOOD2", out)
        self.assertNotIn("CORRUPTED", out)

    def test_corrupted_header_aborts(self):
        self.bus(1, "send", 2, "anything")
        # Flip a byte inside the header body (data_start at offset 24)
        with open(self.bus_path, "r+b") as f:
            f.seek(24)
            b = f.read(1)
            f.seek(24)
            f.write(bytes([b[0] ^ 0xFF]))
        r = self.bus(2, "read", expect_ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("checksum", r.stderr.lower())

    def test_missing_file_fails(self):
        os.unlink(self.bus_path)
        r = self.bus(1, "read", expect_ok=False)
        self.assertNotEqual(r.returncode, 0)


# ============================================================= concurrency ==

class TestConcurrency(MsgbusBase):

    def test_concurrent_sends_all_land(self):
        """MAX_MESSAGES terminals race to send; all should succeed."""
        procs = [self.spawn(tid, "send", 99, f"UNIQUE{tid}_END")
                 for tid in range(1, MAX_MESSAGES + 1)]
        results = self.wait_all(procs)
        for rc, out, err in results:
            self.assertEqual(rc, 0, msg=err)
        out = self.bus(99, "read").stdout
        for tid in range(1, MAX_MESSAGES + 1):
            self.assertIn(f"UNIQUE{tid}_END", out)

    def test_concurrent_sends_respect_capacity(self):
        """15 terminals race; exactly MAX_MESSAGES must succeed."""
        N = 15
        procs = [self.spawn(tid, "send", 99, f"m{tid}")
                 for tid in range(1, N + 1)]
        results = self.wait_all(procs)
        successes = sum(1 for rc, _, _ in results if rc == 0)
        failures  = N - successes
        self.assertEqual(successes, MAX_MESSAGES,
                         msg=f"expected exactly {MAX_MESSAGES} successes")
        self.assertEqual(failures, N - MAX_MESSAGES)
        # Every failed one must have said "full"
        for rc, out, err in results:
            if rc != 0:
                self.assertIn("full", err.lower())

    def test_concurrent_reads_no_duplicates(self):
        """Two readers for the same terminal must not both see a message."""
        for i in range(5):
            self.bus(1, "send", 2, f"UNIQ{i}X")
        p1 = self.spawn(2, "read")
        p2 = self.spawn(2, "read")
        results = self.wait_all([p1, p2])
        combined = results[0][1] + results[1][1]
        for i in range(5):
            self.assertEqual(
                combined.count(f"UNIQ{i}X"), 1,
                f"message UNIQ{i}X appeared {combined.count(f'UNIQ{i}X')} times"
            )

    def test_concurrent_send_and_read_no_crash(self):
        """Mixed sends and reads racing — file stays consistent."""
        for i in range(3):
            self.bus(1, "send", 2, f"pre{i}")
        procs = []
        for tid in (3, 4, 5):
            procs.append(self.spawn(tid, "send", 2, f"from{tid}"))
        procs.append(self.spawn(2, "read"))
        results = self.wait_all(procs)
        for rc, out, err in results:
            self.assertEqual(rc, 0, msg=err)
        # File must still be readable afterwards
        self.bus(2, "read")

    def test_concurrent_mixed_commands(self):
        """Sends, reads, and a clean running together — no corruption."""
        for i in range(3):
            self.bus(1, "send", 2, f"seed{i}")
        procs = [
            self.spawn(3, "send", 2, "AAA"),
            self.spawn(4, "send", 2, "BBB"),
            self.spawn(2, "read"),
            self.spawn(1, "clean"),
        ]
        results = self.wait_all(procs)
        for rc, out, err in results:
            self.assertEqual(rc, 0, msg=err)
        self.bus(2, "read")             # file still valid


# ========================================================= command line ===

class TestCommandLine(MsgbusBase):

    def _run_raw(self, args):
        return subprocess.run(
            [BINARY, *args], cwd=self.tmp.name,
            capture_output=True, text=True,
        )

    def test_no_args(self):
        r = self._run_raw([])
        self.assertNotEqual(r.returncode, 0)

    def test_only_terminal_id(self):
        r = self._run_raw(["1"])
        self.assertNotEqual(r.returncode, 0)

    def test_unknown_command(self):
        r = self._run_raw(["1", "bogus"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown", r.stderr.lower())

    def test_send_missing_message(self):
        r = self._run_raw(["1", "send", "2"])
        self.assertNotEqual(r.returncode, 0)

    def test_send_missing_receiver(self):
        r = self._run_raw(["1", "send"])
        self.assertNotEqual(r.returncode, 0)

    def test_invalid_terminal_id(self):
        r = self._run_raw(["notanumber", "read"])
        self.assertNotEqual(r.returncode, 0)

    def test_negative_terminal_id(self):
        r = self._run_raw(["-3", "read"])
        self.assertNotEqual(r.returncode, 0)

    def test_huge_terminal_id(self):
        r = self._run_raw(["99999999999", "read"])
        self.assertNotEqual(r.returncode, 0)


# ====================================================== file structure ====

class TestFileStructure(MsgbusBase):

    def test_header_magic(self):
        data = self.read_raw()
        magic = struct.unpack_from("<I", data, 0)[0]
        self.assertEqual(magic, 0x4D534742)

    def test_header_version(self):
        data = self.read_raw()
        version = struct.unpack_from("<I", data, 4)[0]
        self.assertEqual(version, 1)

    def test_header_max_messages(self):
        data = self.read_raw()
        mm = struct.unpack_from("<I", data, 8)[0]
        self.assertEqual(mm, MAX_MESSAGES)

    def test_data_start_value(self):
        data = self.read_raw()
        ds = struct.unpack_from("<Q", data, 24)[0]
        self.assertEqual(ds, DATA_START)

    def test_file_grows_after_send(self):
        size0 = os.path.getsize(self.bus_path)
        self.bus(1, "send", 2, "x" * 50)
        size1 = os.path.getsize(self.bus_path)
        self.assertEqual(size1, size0 + 50)

    def test_message_count_after_send(self):
        self.bus(1, "send", 2, "a")
        self.bus(1, "send", 2, "b")
        data = self.read_raw()
        count = struct.unpack_from("<I", data, 12)[0]
        self.assertEqual(count, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)