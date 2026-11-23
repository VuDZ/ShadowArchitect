"""Read the actor table of a Satisfactory blueprint (v2/save 60, zlib).

An offline experiment independent of the UE module. Positions are in centimetres.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
from itertools import product
import math
from pathlib import Path
import struct
import zlib


@dataclass
class Actor:
    kind: str
    rotation: tuple
    position: tuple
    scale: tuple


class Reader:
    def __init__(self, data):
        self.data = memoryview(data)
        self.pos = 0

    def take(self, size):
        end = self.pos + size
        if size < 0 or end > len(self.data):
            raise ValueError("Truncated table")
        block = self.data[self.pos:end]
        self.pos = end
        return block

    def integer(self):
        return struct.unpack("<i", self.take(4))[0]

    def string(self):
        length = self.integer()
        if not length:
            return ""
        width = 1 if length > 0 else 2
        raw = self.take(abs(length) * width)
        if bytes(raw[-width:]) != bytes(width):
            raise ValueError("Invalid FString terminator")
        return bytes(raw[:-width]).decode("utf-8" if width == 1 else "utf-16-le")


def read_actors(data):
    if len(data) < 12 or struct.unpack_from("<ii", data) != (2, 60):
        raise ValueError("Probe supports blueprint version 2, save version 60")
    tag = bytes.fromhex("c1832a9e22222222")
    offset = data.index(tag)
    header = struct.Struct("<8sQBQQQQ")
    raw = bytearray()
    while offset < len(data):
        magic, capacity, codec, packed, size, chunk_packed, chunk_size = header.unpack_from(data, offset)
        start = offset + header.size
        offset = start + packed
        if (magic != tag or codec != 3 or not 0 < size <= capacity <= 131072
                or packed != chunk_packed or size != chunk_size or offset > len(data)):
            raise ValueError("Invalid zlib chunk header")
        if len(raw) + size > 16*1024*1024:
            raise ValueError("Inflated table exceeds probe limit")
        stream = zlib.decompressobj()
        block = stream.decompress(data[start:offset], size + 1)
        if len(block) != size or not stream.eof or stream.unused_data:
            raise ValueError("Invalid compressed payload")
        raw.extend(block)
    payload = Reader(raw)
    if payload.integer() != len(raw) - 4:
        raise ValueError("Wrong payload length")
    table = Reader(payload.take(payload.integer()))
    count = table.integer()
    if count < 0 or count > len(table.data) // 4:
        raise ValueError("Invalid object count")
    actors = []
    for _ in range(count):
        record = table.integer()
        kind = table.string()
        table.string()  # level
        table.string()  # instance path
        table.integer()  # object flags
        if record == 0:
            table.string()  # component outer
        elif record == 1:
            table.integer()  # transform flag
            values = struct.unpack("<10f", table.take(40))
            table.integer()  # actor placement flag
            if not all(map(math.isfinite, values)):
                raise ValueError("Non-finite actor transform")
            actors.append(Actor(kind, values[:4], values[4:7], values[7:]))
        else:
            raise ValueError(f"Unknown object header {record}")
    if table.pos != len(table.data):
        raise ValueError("Header table size mismatch")
    return actors


FOUNDATION = ("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/"
              "Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C")


def transform_point(actor, vertex):
    # Quaternion x,y,z,w and S -> R -> T as in the serialized actor transform.
    x, y, z, w = actor.rotation
    if not math.isclose(x*x + y*y + z*z + w*w, 1, abs_tol=1e-4):
        raise ValueError("Expected a unit quaternion")
    a, b, c = (v*s for v, s in zip(vertex, actor.scale))
    u, v, t = 2*(y*c-z*b), 2*(z*a-x*c), 2*(x*b-y*a)
    rotated = (a+w*u+y*t-z*v, b+w*v+z*u-x*t, c+w*t+x*v-y*u)
    return tuple(r+p for r, p in zip(rotated, actor.position))


def foundation_obj(actors):
    if not actors or any(actor.kind != FOUNDATION for actor in actors):
        raise ValueError("OBJ experiment requires only concrete 8 x 1 m foundations")
    lines = ["# Approximate foundation geometry in cm, Satisfactory Z up"]
    faces = ((1, 2, 4, 3), (5, 7, 8, 6), (1, 5, 6, 2),
             (3, 4, 8, 7), (1, 3, 7, 5), (2, 6, 8, 4))
    for index, actor in enumerate(actors):
        lines.append(f"o Foundation_{index+1}")
        for vertex in product((-400, 400), (-400, 400), (-50, 50)):
            xyz = transform_point(actor, vertex)
            lines.append("v " + " ".join(f"{v:.6f}" for v in xyz))
        for face in faces:
            lines.append("f " + " ".join(str(index*8+i) for i in face))
    return "\n".join(lines) + "\n"


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("blueprint", type=Path)
    cli.add_argument("--obj", type=Path, help="Create a new OBJ for the foundation-only experiment")
    args = cli.parse_args()
    try:
        with args.blueprint.open("rb") as source:
            data = source.read(1024*1024 + 1)
        if len(data) > 1024*1024:
            raise ValueError("File exceeds the 1 MiB probe limit")
        actors = read_actors(data)
        print(f"{len(actors)} actors, {len({a.position for a in actors})} distinct origins")
        for kind, count in Counter(a.kind for a in actors).most_common():
            print(count, kind)
        if args.obj:
            if args.obj.suffix.lower() != ".obj":
                raise ValueError("Output must be a new .obj file")
            model = foundation_obj(actors)
            with args.obj.open("x", encoding="ascii") as out:
                out.write(model)
    except (ValueError, OSError, struct.error, zlib.error) as exc:
        cli.exit(1, f"Probe: {exc}\n")


if __name__ == "__main__":
    main()
