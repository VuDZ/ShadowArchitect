import math
from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from probe_blueprint import Actor, FOUNDATION, foundation_obj, read_actors, transform_point


def integer(value):
    return struct.pack("<i", value)


def string(value):
    data = value.encode() + b"\0"
    return integer(len(data)) + data


def sample():
    prefix = string(FOUNDATION) + string("Level") + string("Example") + integer(8)
    actor = integer(1) + prefix + integer(1)
    actor += struct.pack("<10f", 0, 0, 0, 1, 800, -400, 50, 1, 1, 1) + integer(0)
    component = integer(0) + prefix + string("Parent")
    table = integer(2) + actor + component
    payload = integer(len(table) + 4) + integer(len(table)) + table
    data = bytearray(integer(2) + integer(60) + integer(520140))
    for block in (payload[:32], payload[32:]):
        compressed = zlib.compress(block)
        data += struct.pack("<8sQBQQQQ", bytes.fromhex("c1832a9e22222222"), 131072, 3,
                            len(compressed), len(block), len(compressed), len(block))
        data += compressed
    return data


class ProbeTests(unittest.TestCase):
    def test_chunks_and_component_filter(self):
        actors = read_actors(sample())
        self.assertEqual(len(actors), 1)
        self.assertEqual(actors[0].kind, FOUNDATION)
        self.assertEqual(actors[0].position, (800, -400, 50))

    def test_reject_incomplete_or_unknown_data(self):
        data = sample()
        for invalid in (data[:-1], integer(3) + data[4:], data + b"garbage"*8):
            with self.assertRaises((ValueError, struct.error)):
                read_actors(invalid)

    def test_proxy_rotation_scale_and_independent_objects(self):
        r = math.sqrt(.5)
        actor = Actor(FOUNDATION, (0, 0, r, r), (10, 20, 30), (2, 1, 1))
        for actual, expected in zip(transform_point(actor, (1, 0, 0)), (10, 22, 30)):
            self.assertAlmostEqual(actual, expected)
        model = foundation_obj([actor, actor]).splitlines()
        self.assertEqual(sum(line.startswith("o ") for line in model), 2)
        self.assertEqual(sum(line.startswith("v ") for line in model), 16)
        self.assertEqual(model[-1], "f 10 14 16 12")
        with self.assertRaises(ValueError):
            foundation_obj([Actor("unknown", actor.rotation, actor.position, actor.scale)])
