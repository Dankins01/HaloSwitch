#!/usr/bin/env python3
"""Builds h2weapons.pak, the Halo 2 weapons the port adds to Halo's maps
(port/linux/game/weapon_tags.c), from the player's own copy of Halo 2 (the
Xbox game's maps: shared.map, and mainmenu.map, which holds their geometry
and textures).

    python3 tools/h2_weapons.py HALO2_MAPS_FOLDER OUTPUT.pak

For each weapon: its first-person and third-person models (Halo 2's
render models: each section's vertices and strips, re-encoded as Halo's
compressed model vertices, in the space of the Halo weapon it is built on:
the donor), the textures their shaders draw with (Halo 2's DXT data as it
is), and the stats the game gives it. The donor's nodes, animations,
sounds and HUD are the game's: a Halo 2 node is bound to the donor's of
the same name ("magazine" to "frame magazine"), else to its gun.

Nothing of Halo 2's is kept in this repository: the pack is made from the
player's files, on their machine.
"""

import io
import os
import struct
import sys

# ---------- Halo 2 Xbox cache maps (retail, version 8)


class H2Map:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        u = lambda o: struct.unpack_from("<I", d, o)[0]
        if d[:4] != b"daeh" or u(4) != 8:
            raise ValueError(f"{path}: not a Halo 2 Xbox map")
        self.index_offset, self.index_size = u(0x10), u(0x14)
        idx = self.index_offset
        _, group_count, _, _, _, _, tag_count, magic = struct.unpack_from("<8I", d, idx)
        if magic != 0x74616773:
            raise ValueError(f"{path}: no tag index")
        table = idx + 32 + group_count * 12
        self.tags = []
        for i in range(tag_count):
            g, tid, addr, size = struct.unpack_from("<4sIII", d, table + i * 16)
            self.tags.append([g[::-1].decode("latin-1"), tid, addr, size, None])
        count, names, index = u(0x2C0), u(0x2C4), u(0x2CC)
        for i in range(min(count, tag_count)):
            o = u(index + i * 4)
            self.tags[i][4] = d[names + o:d.index(b"\0", names + o)].decode("latin-1")
        first = next(t for t in self.tags if t[2])
        self.magic = first[2] - (idx + self.index_size)
        self.by_id = {t[1]: t for t in self.tags}
        self.sid_count, self.sid_index, self.sid_table = u(0x164), u(0x16C), u(0x170)

    def offset(self, address):
        return address - self.magic

    def tag(self, group, name):
        for t in self.tags:
            if t[0] == group and t[4] == name:
                return t
        raise KeyError(f"{group} {name}")

    def base(self, tag):
        return self.offset(tag[2])

    def block(self, offset):
        count, address = struct.unpack_from("<II", self.data, offset)
        return count, (self.offset(address) if count else 0)

    def string_id(self, sid):
        index = sid & 0xFFFFFF
        if index >= self.sid_count:
            return ""
        o = struct.unpack_from("<I", self.data, self.sid_index + index * 4)[0]
        start = self.sid_table + o
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")


class H2Maps:
    """a map and the maps its raw data can be in: a raw pointer's top bits,
    0 the map's own, 1 mainmenu.map, 2 shared.map, 3 single_player_shared.map"""

    FILES = {1: "mainmenu.map", 2: "shared.map", 3: "single_player_shared.map"}

    def __init__(self, folder, name="shared.map"):
        self.folder = folder
        self.map = H2Map(os.path.join(folder, name))
        self.files = {}

    def raw(self, pointer, size):
        location = pointer >> 30
        if location == 0:
            data = self.map.data
        else:
            if location not in self.files:
                self.files[location] = open(os.path.join(self.folder, self.FILES[location]), "rb").read()
            data = self.files[location]
        offset = pointer & 0x3FFFFFFF
        return data[offset:offset + size]


# ---------- render models


def unpack_vector(v):
    """an 11:11:10 signed packed vector"""
    x, y, z = v & 0x7FF, (v >> 11) & 0x7FF, (v >> 22) & 0x3FF
    x -= 0x800 if x & 0x400 else 0
    y -= 0x800 if y & 0x400 else 0
    z -= 0x400 if z & 0x200 else 0
    return (x / 1023.0, y / 1023.0, z / 511.0)


def pack_vector(v):
    """Halo's compress_real_vector3d_to_int32_clamp"""
    import math
    i = int(math.floor(max(-1.0, min(1.0, v[0])) * 1023.5)) & 0x7FF
    j = int(math.floor(max(-1.0, min(1.0, v[1])) * 1023.5)) & 0x7FF
    k = int(math.floor(max(-1.0, min(1.0, v[2])) * 511.5)) & 0x3FF
    return (((k << 11) | j) << 11) | i


def compress_int16(value):
    """Halo's compress_real_to_int16_clamp"""
    import math
    return max(-32768, min(32767, int(math.floor(max(-1.0, min(1.0, value)) * 32767.5))))


class H2Model:
    SECTION_SIZE = 0x5C
    PART_SIZE = 0x48

    def __init__(self, maps, name):
        self.maps = maps
        m = maps.map
        d = m.data
        base = m.base(m.tag("mode", name))
        _, co = m.block(base + 0x14)
        b = struct.unpack_from("<10f", d, co)
        self.bounds = [(b[0], b[1]), (b[2], b[3]), (b[4], b[5]), (b[6], b[7]), (b[8], b[9])]
        count, no = m.block(base + 0x48)
        self.nodes = []
        for i in range(count):
            o = no + i * 0x60
            self.nodes.append((m.string_id(struct.unpack_from("<I", d, o)[0]), struct.unpack_from("<3f", d, o + 12)))
        count, mo = m.block(base + 0x60)
        self.materials = []
        for i in range(count):
            tid = struct.unpack_from("<I", d, mo + i * 32 + 12)[0]
            self.materials.append(m.by_id[tid][4] if tid in m.by_id else None)
        count, so = m.block(base + 0x24)
        self.sections = [self.section(so + i * self.SECTION_SIZE) for i in range(count)]
        # each region's first permutation: its section at each of Halo 2's six
        # levels of detail (super low to hollywood)
        count, ro = m.block(base + 0x1C)
        self.regions = []
        for i in range(count):
            permutations, po = m.block(ro + i * 16 + 8)
            if permutations:
                self.regions.append(struct.unpack_from("<6h", d, po + 4))

    def decompress(self, axis, value):
        lo, hi = self.bounds[axis]
        return lo + (value + 32768) / 65535.0 * (hi - lo)

    def section(self, so):
        m = self.maps.map
        d = m.data
        vertex_count = struct.unpack_from("<H", d, so + 4)[0]
        raw_pointer, raw_size, header_size, _ = struct.unpack_from("<4I", d, so + 0x38)
        count, address = struct.unpack_from("<II", d, so + 0x48)
        raw = self.maps.raw(raw_pointer, raw_size)
        base = header_size + 8
        blocks, buffers = {}, {}
        for r in range(count):
            kind, _, locator, _, extra, size, offset = struct.unpack_from("<BBHHHII", d, m.offset(address) + r * 16)
            blob = raw[base + offset:base + offset + size]
            if kind == 2:
                buffers[extra] = blob
            else:
                blocks[locator] = blob
        parts = []
        blob = blocks.get(0x00, b"")
        for i in range(len(blob) // self.PART_SIZE):
            _, _, material, start, length = struct.unpack_from("<5H", blob, i * self.PART_SIZE)
            parts.append({"material": material, "start": start, "length": length})
        strips = blocks.get(0x20, b"")
        indices = list(struct.unpack_from(f"<{len(strips) // 2}H", strips))
        node_map = list(blocks.get(0x64, b"\0"))
        descriptors = blocks.get(0x38, b"")
        stride = descriptors[1] if len(descriptors) > 1 else 6
        vertices = []
        positions, uvs, frames = buffers[0], buffers.get(1, b""), buffers.get(2, b"")
        for i in range(vertex_count):
            x, y, z = struct.unpack_from("<3h", positions, i * stride)
            node = node_map[positions[i * stride + 6]] if stride >= 8 and positions[i * stride + 6] < len(node_map) \
                else node_map[0]
            u, v = struct.unpack_from("<2h", uvs, i * 4) if uvs else (0, 0)
            normal, binormal, tangent = (unpack_vector(struct.unpack_from("<I", frames, i * 12 + k * 4)[0])
                                         for k in range(3)) if frames else ((0, 0, 1), (0, 1, 0), (1, 0, 0))
            vertices.append({"position": (self.decompress(0, x), self.decompress(1, y), self.decompress(2, z)),
                             "uv": (self.decompress(3, u), self.decompress(4, v)), "normal": normal,
                             "binormal": binormal, "tangent": tangent, "node": node})
        for part in parts:
            part["strip"] = indices[part["start"]:part["start"] + part["length"]]
        return {"vertices": vertices, "parts": parts}


# ---------- shaders and bitmaps


def shader_base_bitmap(m, shader_name):
    """a shader's base map: the first bitmap it names that is not a bump
    map nor one of the defaults"""
    t = m.tag("shad", shader_name)
    base = m.base(t)
    for o in range(base, base + t[3] - 3, 4):
        tid = struct.unpack_from("<I", m.data, o)[0]
        ref = m.by_id.get(tid)
        if ref and ref[0] == "bitm" and ref[4]:
            name = ref[4]
            if "bump" in name or name.startswith("shaders\\") or name.startswith("rasterizer\\"):
                continue
            return name
    return None


def shader_template(m, shader_name):
    """the name of a shader's template (its first reference)"""
    t = m.tag("shad", shader_name)
    ref = m.by_id.get(struct.unpack_from("<I", m.data, m.base(t) + 4)[0])
    return ref[4] if ref and ref[4] else ""


DXT_FORMATS = {14: 8, 15: 16, 16: 16}
# what a part whose shader has no texture of its own is drawn with (Halo 2's
# BR grips, a tinted default): a 4x4 DXT1 block of dark grey
SOLID_TEXTURE = "h2weapons\\solid dark"
SOLID_DATA = struct.pack("<HHI", 0x2104, 0x2104, 0)


def bitmap_data(maps, name):
    """a bitmap's first image: (width, height, format, levels, the DXT data
    of its levels, level 0 first)"""
    if name == SOLID_TEXTURE:
        return 4, 4, 14, 1, SOLID_DATA
    m = maps.map
    base = m.base(m.tag("bitm", name))
    _, o = m.block(base + 0x44)
    d = m.data
    width, height = struct.unpack_from("<2H", d, o + 4)
    fmt = struct.unpack_from("<H", d, o + 0x0C)[0]
    mipmaps = struct.unpack_from("<H", d, o + 0x14)[0]
    offset = struct.unpack_from("<I", d, o + 0x1C)[0]
    size = struct.unpack_from("<I", d, o + 0x34)[0]
    if fmt not in DXT_FORMATS:
        raise ValueError(f"{name}: format {fmt} is not DXT")
    raw = maps.raw(offset, size)
    block = DXT_FORMATS[fmt]
    levels, total, w, h = 0, 0, width, height
    while levels <= mipmaps:
        total += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
        levels += 1
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return width, height, fmt, levels, raw[:total]


# ---------- the weapons

# Halo 2's node names, and the donor's they are bound to
NODE_NAMES = {"gun": "frame gun", "magazine": "frame magazine", "ophandle": "frame ophandle",
              "safety": "frame safety button", "bullet": "frame bullet", "pump": "frame pump"}
# the materials whose parts are left out: the ammunition counters' digits,
# and the scopes' glass
SKIPPED_MATERIALS = ("\\tens", "\\ones", "numbers", "_lens")
# a part's flags (weapon_tags.c)
PART_ALPHA_TESTED = 1
# Halo's five levels of detail (super low to super high), each drawn as
# Halo 2's level above it: Halo 2's super low is left out
LEVELS = (1, 2, 3, 4, 5)

WEAPONS = [
    {
        "tag": "weapons\\h2 battle rifle\\battle rifle", "display": "BATTLE RIFLE",
        "donor": "weapons\\assault rifle\\assault rifle",
        "first_person": "objects\\weapons\\rifle\\battle_rifle\\fp_battle_rifle\\fp_battle_rifle",
        "third_person": "objects\\weapons\\rifle\\battle_rifle\\battle_rifle",
        # (a three-round burst: three bullets a shot, three shots a second)
        "magazine": 36, "maximum": 108, "rounds_per_second": 3.0, "projectiles_per_shot": 3, "rounds_per_shot": 3,
        "error": 0.25, "zoom": 2.0, "damage": 1.6,
    },
    {
        "tag": "weapons\\h2 smg\\smg", "display": "SMG",
        "donor": "weapons\\assault rifle\\assault rifle",
        "first_person": "objects\\weapons\\rifle\\smg\\fp_smg\\fp_smg",
        "third_person": "objects\\weapons\\rifle\\smg\\smg",
        "magazine": 60, "maximum": 240, "rounds_per_second": 15.0, "projectiles_per_shot": 1, "rounds_per_shot": 1,
        "error": 1.4, "zoom": 0.0, "damage": 0.85,
    },
    {
        "tag": "weapons\\h2 shotgun\\shotgun", "display": "H2 SHOTGUN",
        "donor": "weapons\\shotgun\\shotgun",
        "first_person": "objects\\weapons\\rifle\\shotgun\\fp_shotgun\\fp_shotgun",
        "third_person": "objects\\weapons\\rifle\\shotgun\\shotgun",
        "magazine": 0, "maximum": 0, "rounds_per_second": 0.0, "projectiles_per_shot": 0, "rounds_per_shot": 0,
        "error": 1.0, "zoom": 0.0, "damage": 1.0,
    },
]


# (weapon_tags.c reads this version)
PACK_VERSION = 2


def name_bytes(text, size):
    data = text.encode("latin-1")[:size - 1]
    return data + b"\0" * (size - len(data))


def part_record(maps, model, section, part, local, gun, textures):
    material = model.materials[part["material"]] if part["material"] < len(model.materials) else None
    if not part["strip"] or (material and any(k in material for k in SKIPPED_MATERIALS)):
        return None
    bitmap = (shader_base_bitmap(maps.map, material) if material else None) or SOLID_TEXTURE
    flags = PART_ALPHA_TESTED if material and "alpha_test" in shader_template(maps.map, material) else 0
    if bitmap not in textures:
        textures[bitmap] = len(textures)
    # (the part's vertices only, renumbered)
    used = sorted(set(part["strip"]))
    renumber = {v: i for i, v in enumerate(used)}
    vertices = bytearray()
    for v in used:
        vertex = section["vertices"][v]
        x, y, z = vertex["position"]
        # (in the donor's space: its gun at the model's origin)
        position = (x - gun[0], y - gun[1], z - gun[2])
        node = local[vertex["node"]] if vertex["node"] < len(local) else 0
        vertices += struct.pack("<3f3I2hBBh", *position, pack_vector(vertex["normal"]),
                                pack_vector(vertex["binormal"]), pack_vector(vertex["tangent"]),
                                compress_int16(vertex["uv"][0]), compress_int16(vertex["uv"][1]),
                                node, 0xFF, 32767)
    strip = [renumber[i] for i in part["strip"]]
    strip = [strip[0]] + strip
    return (struct.pack("<hHII", textures[bitmap], flags, len(used), len(strip)) + bytes(vertices) +
            struct.pack(f"<{len(strip)}H", *strip))


def model_record(maps, name, textures):
    """a model: its node names (the donor's they are bound to), the geometry
    each of Halo's levels of detail draws, and the geometries: each its
    regions' sections' parts, each a texture, compressed vertices and a
    strip (in Halo's winding: one index more in front)"""
    model = H2Model(maps, name)
    gun = next((t for n, t in model.nodes if n == "gun"), (0.0, 0.0, 0.0))
    node_names = []
    node_index = {}
    for n, _ in model.nodes:
        mapped = NODE_NAMES.get(n, "frame gun")
        if mapped not in node_index:
            node_index[mapped] = len(node_names)
            node_names.append(mapped)
    local = [node_index[NODE_NAMES.get(n, "frame gun")] for n, _ in model.nodes]
    regions = model.regions or [tuple([i] * 6) for i in range(len(model.sections))]
    geometries, levels = [], []
    for level in LEVELS:
        sections = tuple(sorted({r[level] for r in regions if 0 <= r[level] < len(model.sections)}))
        if sections not in geometries:
            geometries.append(sections)
        levels.append(geometries.index(sections))
    record = struct.pack("<H", len(node_names)) + b"".join(name_bytes(n, 32) for n in node_names)
    record += struct.pack("<5BxH", *levels, len(geometries))
    for sections in geometries:
        parts = [p for s in sections for p in (part_record(maps, model, model.sections[s], part, local, gun, textures)
                                               for part in model.sections[s]["parts"]) if p]
        record += struct.pack("<H", len(parts)) + b"".join(parts)
    return record


def build(folder, output):
    maps = H2Maps(folder)
    textures = {}
    records = []
    for weapon in WEAPONS:
        fp = model_record(maps, weapon["first_person"], textures)
        tp = model_record(maps, weapon["third_person"], textures)
        records.append(name_bytes(weapon["tag"], 64) + name_bytes(weapon["donor"], 64) +
                       name_bytes(weapon["display"], 32) +
                       struct.pack("<hhfhhfff", weapon["magazine"], weapon["maximum"], weapon["rounds_per_second"],
                                   weapon["projectiles_per_shot"], weapon["rounds_per_shot"], weapon["error"],
                                   weapon["zoom"], weapon["damage"]) + fp + tp)
    texture_records = []
    for name, _ in sorted(textures.items(), key=lambda item: item[1]):
        width, height, fmt, levels, data = bitmap_data(maps, name)
        texture_records.append(name_bytes(name, 64) + struct.pack("<HHHHI", width, height, fmt, levels, len(data)) +
                               data)
    with open(output, "wb") as f:
        f.write(b"H2WP" + struct.pack("<III", PACK_VERSION, len(records), len(texture_records)))
        for record in texture_records:
            f.write(record)
        for record in records:
            f.write(record)
    print(f"{output}: {len(records)} weapons, {len(texture_records)} textures, {os.path.getsize(output) // 1024} KB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])
