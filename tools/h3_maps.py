"""Halo 3's Xbox 360 maps (cache version 11), read for tools/h2_weapons.py.

It reads what a first-person weapon needs from your own copy of Halo 3's
maps (nothing of Halo 3's is in this repository): the render models (their
nodes, meshes and parts, from the models' geometry resources), the shaders'
base maps and the bitmaps (untiled from the Xbox 360's layout). A level's
map names its tags; their resources are in its own resource section or in
the shared maps (mainmenu.map, shared.map) beside it.

The maps are big-endian. Virtual addresses in the tag data are the tag
buffer's (its first partition's), and a resource's definition points into
its pages with fixups (an address's top bits: 2 the definition, 4 the
primary page, 6 the secondary).
"""

import mmap
import os

import numpy as np
import struct
import zlib

# the shared maps a resource page can be in (the layout table's own list)
SHARED_MAPS = ("mainmenu.map", "shared.map")


class H3Map:
    """a map: its sections, string ids, tag names and tag index"""

    def __init__(self, path, tags=True):
        self.path = path
        self.file = open(path, "rb")
        self.m = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if self.m[:4] != b"head" or self.u32(4) != 11:
            raise ValueError(f"{path}: not a Halo 3 map (cache version 11)")
        # sections (debug, resource, tag, locale): their file offsets, each
        # the header's bounds plus its mask
        masks = [self.u32(0x46C + 4 * i) for i in range(4)]
        bounds = [(self.u32(0x47C + 8 * i), self.u32(0x480 + 8 * i)) for i in range(4)]
        self.sections = [((b + k) & 0xFFFFFFFF, s) for k, (b, s) in zip(masks, bounds)]
        self.debug_mask = masks[0]
        self.base = self.u32(0x2E8)
        if not tags:
            return
        self.strings = self.string_table(0x158, 0x160, 0x164)
        names = self.string_table(0x2B4, 0x2C0, 0x2B8)
        index = self.offset(self.u32(0x10))
        group_count, group_address, tag_count, tag_address = struct.unpack(">IIII", self.m[index:index + 16])
        g = self.offset(group_address)
        self.groups = [self.m[g + 16 * i:g + 16 * i + 4].decode("latin-1") for i in range(group_count)]
        t = self.offset(tag_address)
        self.tags = []
        self.by_name = {}
        for i in range(tag_count):
            group, _, address = struct.unpack(">HHI", self.m[t + 8 * i:t + 8 * i + 8])
            if group == 0xFFFF:
                self.tags.append(None)
                continue
            entry = (self.groups[group], names[i] if i < len(names) else "", address)
            self.tags.append(entry)
            self.by_name[entry[:2]] = entry

    def string_table(self, count_at, index_at, data_at):
        count = self.u32(count_at)
        index = (self.u32(index_at) + self.debug_mask) & 0xFFFFFFFF
        data = (self.u32(data_at) + self.debug_mask) & 0xFFFFFFFF
        table = []
        for i in range(count):
            o = data + self.u32(index + 4 * i)
            table.append(self.m[o:self.m.find(b"\0", o)].decode("latin-1"))
        return table

    def u32(self, o):
        return struct.unpack(">I", self.m[o:o + 4])[0]

    def offset(self, address):
        return self.sections[2][0] + ((address - self.base) & 0xFFFFFFFF)

    def tag(self, group, name):
        entry = self.by_name.get((group, name))
        return self.offset(entry[2]) if entry else None

    def block(self, o):
        count, address = struct.unpack(">II", self.m[o:o + 8])
        return count, (self.offset(address) if count else 0)

    def reference(self, o):
        """a tag reference's (group, name), else None"""
        index = struct.unpack(">I", self.m[o + 12:o + 16])[0]
        i = index & 0xFFFF
        return self.tags[i][:2] if index != 0xFFFFFFFF and i < len(self.tags) and self.tags[i] else None

    def string_id(self, value):
        """Halo 3's string ids: the global set's after the others' (1882)"""
        if value == 0:
            return ""
        i = (value & 0xFFFF) + 0x75A if not (value >> 16) & 0xFF else value & 0xFFFF
        return self.strings[i] if i < len(self.strings) else f"sid{value:x}"


class H3Resources:
    """a map's resources: the zone tag's entries, the layout table's pages"""

    def __init__(self, map_):
        self.map = map_
        m = map_
        self.zone = m.offset([t for t in m.tags if t and t[0] == "zone"][0][2])
        self.play = m.offset([t for t in m.tags if t and t[0] == "play"][0][2])
        self.definitions = m.offset(m.u32(self.zone + 0x148))
        self.folder = os.path.dirname(m.path)
        self.shared = {}
        self.pages = {}

    def shared_map(self, index):
        if index not in self.shared:
            self.shared[index] = H3Map(os.path.join(self.folder, SHARED_MAPS[index]), tags=False)
        return self.shared[index]

    def page(self, index):
        if index < 0:
            return None
        if index not in self.pages:
            m = self.map
            o = m.block(self.play + 0x18)[1] + 0x58 * index
            _, _, codec, shared, _, offset, compressed, _ = struct.unpack(">HBBHHIII", m.m[o:o + 20])
            source = m if shared == 0xFFFF else self.shared_map(shared)
            start = source.sections[1][0] + offset
            raw = source.m[start:start + compressed]
            self.pages[index] = zlib.decompress(raw, -15) if codec == 0 else bytes(raw)
        return self.pages[index]

    def get(self, datum):
        """a resource: its definition (fixups applied: a dict of offset to
        (kind, offset)), the primary and secondary data"""
        m = self.map
        e = m.block(self.zone + 0x58)[1] + 0x40 * (datum & 0xFFFF)
        offset, size = m.u32(e + 0x14), m.u32(e + 0x18)
        segment = struct.unpack(">H", m.m[e + 0x22:e + 0x24])[0]
        definition = bytes(m.m[self.definitions + offset:self.definitions + offset + size])
        fixups = {}
        count, o = m.block(e + 0x28)
        for i in range(count):
            at, address = struct.unpack(">II", m.m[o + 8 * i:o + 8 * i + 8])
            fixups[at] = (address >> 28, address & 0x0FFFFFFF)
        s = m.block(self.play + 0x30)[1] + 16 * segment
        primary, secondary, primary_offset, secondary_offset = struct.unpack(">hhii", m.m[s:s + 12])
        a, b = self.page(primary), self.page(secondary)
        return (definition, fixups, a[primary_offset:] if a is not None else None,
                b[secondary_offset:] if b is not None else None)


# ---------- the Xbox 360's textures


def tiled_index(x, y, width, log_bpb):
    """XGAddress2DTiledOffset: the tiled index of a texel (here a block)"""
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpb
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpb)) + ((y & 1) << 4)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpb


def untile_blocks(data, width_blocks, height_blocks, block_size):
    """tiled big-endian blocks to linear little-endian ones (each 16-bit
    word swapped)"""
    log = {8: 3, 16: 4}[block_size]
    out = bytearray(width_blocks * height_blocks * block_size)
    for y in range(height_blocks):
        for x in range(width_blocks):
            s = tiled_index(x, y, width_blocks, log) * block_size
            blk = data[s:s + block_size]
            if len(blk) < block_size:
                continue
            d = (y * width_blocks + x) * block_size
            out[d:d + block_size:2] = blk[1::2]
            out[d + 1:d + block_size:2] = blk[0::2]
    return bytes(out)


# ---------- render models


def dhen3n(word):
    """D3DDECLTYPE_DHEN3N: 10:11:11 signed"""
    def signed(v, bits):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v & (1 << (bits - 1)) else v
    return (signed(word, 10) / 511.0, signed(word >> 10, 11) / 1023.0, signed(word >> 21, 11) / 1023.0)


def strip_from_list(indices):
    """a triangle list as one strip (degenerate joins)"""
    strip = []
    for i in range(0, len(indices) - 2, 3):
        a, b, c = indices[i:i + 3]
        if strip:
            strip += [strip[-1], a]
            if len(strip) % 2:
                strip.append(a)
        strip += [a, b, c]
    return strip


def matrix_inverse(m):
    """a rigid matrix4x3's (forward, left, up, position) inverse"""
    f, l, u, p = m
    rows = (f, l, u)
    forward = (f[0], l[0], u[0])
    left = (f[1], l[1], u[1])
    up = (f[2], l[2], u[2])
    position = tuple(-sum(rows[r][c] * p[c] for c in range(3)) for r in range(3))
    return (forward, left, up, position)


class H3Model:
    """a render model, as tools/h2_weapons.py's H2Model has one: its nodes,
    their binds, the materials (shader names), the meshes (as sections:
    vertices and parts) and its regions' meshes"""

    def __init__(self, maps, name):
        m = maps.map
        self.maps = maps
        base = m.tag("mode", name)
        if base is None:
            raise KeyError(f"no render model {name}")
        count, no = m.block(base + 0x30)
        self.nodes, self.binds = [], []
        for i in range(count):
            o = no + 0x60 * i
            node_name = m.string_id(m.u32(o))
            translation = struct.unpack(">3f", m.m[o + 0x0C:o + 0x18])
            inverse = struct.unpack(">12f", m.m[o + 0x2C:o + 0x5C])
            self.nodes.append((node_name, translation))
            self.binds.append(matrix_inverse((inverse[0:3], inverse[3:6], inverse[6:9], inverse[9:12])))
        count, mo = m.block(base + 0x48)
        self.materials = [(m.reference(mo + 0x24 * i) or (None, None))[1] for i in range(count)]
        # the geometry: its compression bounds, meshes and resource
        count, co = m.block(base + 0x74)
        b = struct.unpack(">10f", m.m[co + 4:co + 44]) if count else (-1, 1, -1, 1, -1, 1, 0, 1, 0, 1)
        self.bounds = [(b[0], b[1]), (b[2], b[3]), (b[4], b[5]), (b[6], b[7]), (b[8], b[9])]
        definition, fixups, primary, _ = maps.resources.get(m.u32(base + 0xE0))
        vertex_buffers, index_buffers = self.buffers(definition, fixups, primary)
        mesh_count, meo = m.block(base + 0x68)
        self.sections = []
        for i in range(mesh_count):
            self.sections.append(self.mesh(meo + 0x4C * i, vertex_buffers, index_buffers))
        count, ro = m.block(base + 0x0C)
        self.regions = []
        for i in range(count):
            permutations, po = m.block(ro + 0x10 * i + 4)
            if permutations:
                mesh = struct.unpack(">h", m.m[po + 4:po + 6])[0]
                self.regions.append(tuple([mesh] * 6))

    def buffers(self, definition, fixups, primary):
        """the resource's vertex buffers (count, stride, data) and index
        buffers (kind, data)"""
        def pointer(at):
            kind, offset = fixups.get(at, (0, 0))
            return kind, offset

        def block(at):
            count = struct.unpack(">I", definition[at:at + 4])[0]
            return count, pointer(at + 4)[1]
        # the root: the vertex buffers' and index buffers' blocks (each entry a
        # pointer to its D3D header in the definition)
        root = None
        for at in sorted(fixups):
            pass
        # (the root is the definition's last 0x18 bytes' blocks: find the two
        # blocks whose entries point at headers)
        vertex_buffers, index_buffers = [], []
        for at in range(0, len(definition) - 0x18 + 1, 4):
            vc, va = struct.unpack(">II", definition[at:at + 8])
            ic = struct.unpack(">I", definition[at + 12:at + 16])[0]
            if at + 4 in fixups and at + 16 in fixups and fixups[at + 4][0] == 2 and fixups[at + 16][0] == 2 and \
                    0 < vc < 64 and 0 < ic < 64:
                root = at
        if root is None:
            raise ValueError("geometry resource: no vertex buffers")
        vc, vo = block(root)
        ic, io = block(root + 12)
        for i in range(vc):
            header = pointer(vo + 12 * i)[1]
            count, kind_stride = struct.unpack(">II", definition[header:header + 8])
            data_kind, data_offset = pointer(header + 0x14)
            vertex_buffers.append((count, kind_stride & 0xFFFF, primary[data_offset:]))
        for i in range(ic):
            header = pointer(io + 12 * i)[1]
            kind, size = struct.unpack(">II", definition[header:header + 8])
            data_kind, data_offset = pointer(header + 0x10)
            index_buffers.append((kind, primary[data_offset:data_offset + size]))
        return vertex_buffers, index_buffers

    def decompress(self, axis, value):
        lo, hi = self.bounds[axis]
        return lo + value / 65535.0 * (hi - lo)

    def mesh(self, o, vertex_buffers, index_buffers):
        m = self.maps.map
        streams = struct.unpack(">8h", m.m[o + 0x18:o + 0x28])
        index_buffer = struct.unpack(">h", m.m[o + 0x28:o + 0x2A])[0]
        rigid_node, vertex_type = m.m[o + 0x2D], m.m[o + 0x2E]
        vertices = []
        if streams[0] >= 0:
            count, stride, data = vertex_buffers[streams[0]]
            for i in range(count):
                v = data[i * stride:(i + 1) * stride]
                x, y, z = struct.unpack(">3H", v[0:6])
                u, t = struct.unpack(">2H", v[8:12])
                normal, tangent, binormal = (dhen3n(struct.unpack(">I", v[12 + 4 * k:16 + 4 * k])[0]) for k in range(3))
                if vertex_type == 2 and stride >= 32:
                    nodes, weights = v[24:28], v[28:32]
                    node = nodes[max(range(4), key=lambda k: weights[k])]
                else:
                    node = rigid_node if rigid_node != 0xFF else 0
                vertices.append({"position": (self.decompress(0, x), self.decompress(1, y), self.decompress(2, z)),
                                 "uv": (self.decompress(3, u), self.decompress(4, t)), "normal": normal,
                                 "binormal": binormal, "tangent": tangent, "node": node})
        indices = []
        if index_buffer >= 0:
            kind, data = index_buffers[index_buffer]
            indices = list(struct.unpack(f">{len(data) // 2}H", data))
        count, po = m.block(o)
        parts = []
        for i in range(count):
            material, _, start, length = struct.unpack(">hhHH", m.m[po + 0x10 * i:po + 0x10 * i + 8])
            piece = indices[start:start + length]
            strip = piece if not index_buffers or index_buffers[index_buffer][0] == 5 else strip_from_list(piece)
            parts.append({"material": material, "start": start, "length": length, "strip": strip})
        return {"vertices": vertices, "parts": parts}


class H3Maps:
    """a level's map and its shared maps' resources, as tools/h2_weapons.py
    asks them: models, shaders' base maps and bitmaps"""

    # Halo 3's bitmap formats that the pack takes (Halo's codes)
    DXT = {14: (14, 8), 15: (15, 16), 16: (16, 16)}

    def __init__(self, folder, level):
        self.map = H3Map(os.path.join(folder, level))
        self.resources = H3Resources(self.map)

    def model(self, name):
        return H3Model(self, name)

    def shader_base_bitmap(self, shader_name):
        """a shader's base map: its postprocess's first bitmap"""
        m = self.map
        o = m.tag("rmsh", shader_name)
        if o is None:
            return None
        count, po = m.block(o + 0x28)
        if not count:
            return None
        count, bo = m.block(po + 0x10)
        for i in range(count):
            ref = m.reference(bo + 0x18 * i)
            if ref and ref[0] == "bitm" and not ref[1].startswith("shaders\\"):
                return ref[1]
        return None

    def shader_template(self, shader_name):
        m = self.map
        o = m.tag("rmsh", shader_name)
        if o is None:
            return ""
        count, po = m.block(o + 0x28)
        ref = m.reference(po) if count else None
        return ref[1] if ref else ""

    def bitmap_data(self, name):
        """a bitmap's first image, its first level: (width, height, Halo's
        format, 1, the DXT data)"""
        m = self.map
        o = m.tag("bitm", name)
        count, io = m.block(o + 0x60)
        width, height = struct.unpack(">HH", m.m[io + 4:io + 8])
        fmt = struct.unpack(">H", m.m[io + 0x0C:io + 0x0E])[0]
        if fmt not in self.DXT:
            raise ValueError(f"{name}: format {fmt} is not DXT")
        count, ro = m.block(o + 0x8C)
        definition, fixups, primary, secondary = self.resources.get(m.u32(ro))
        halo_format, block = self.DXT[fmt]
        wb, hb = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
        # (the first level: the secondary page's when it has one, else the
        # primary's first)
        source = secondary if secondary is not None and len(secondary) >= wb * hb * block else primary
        aligned = ((wb + 31) & ~31) * ((hb + 31) & ~31) * block
        data = untile_blocks(source[:max(aligned, wb * hb * block)], wb, hb, block)
        # (resampled to a power of two, with mipmaps: the port's textures
        # must be; DXT3 as DXT5)
        image = dxt_decode(data, width, height, halo_format)
        if halo_format == 15:
            halo_format = 16
        levels, data = power_of_two_levels(image, halo_format)
        size = 1 << (max(width, 4).bit_length() - 1), 1 << (max(height, 4).bit_length() - 1)
        return size[0], size[1], halo_format, levels, data


# ---------- DXT, for resampling Halo 3's textures that are not a power of
# two in size (the port's textures must be)


def _565(colors):
    c = colors.astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)


def _rgb(c):
    c = c.astype(np.int32)
    return np.stack([(c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def dxt_decode(data, width, height, fmt):
    """linear DXT1/3/5 blocks to RGBA (height, width, 4)"""
    bs = 8 if fmt == 14 else 16
    wb, hb = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    blocks = np.frombuffer(data[:wb * hb * bs], np.uint8).reshape(hb * wb, bs)
    color = blocks[:, bs - 8:]
    c0 = color[:, 0].astype(np.int32) | (color[:, 1].astype(np.int32) << 8)
    c1 = color[:, 2].astype(np.int32) | (color[:, 3].astype(np.int32) << 8)
    bits = color[:, 4:8].astype(np.uint32)
    bits = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    p0, p1 = _rgb(c0), _rgb(c1)
    four = (c0 > c1) | (fmt != 14)
    palette = np.stack([p0, p1,
                        np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2),
                        np.where(four[:, None], (p0 + 2 * p1) // 3, 0)], 1)
    index = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(palette, index[:, :, None].astype(np.int64).repeat(3, 2), 1)
    alpha = np.full((len(blocks), 16), 255, np.int32)
    if fmt == 16:
        a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
        abits = np.zeros(len(blocks), np.uint64)
        for k in range(6):
            abits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        eight = a0 > a1
        table = [a0, a1] + [np.where(eight, (a0 * (7 - i) + a1 * i) // 7,
                                     (a0 * (5 - i) + a1 * i) // 5 if i < 5 else (0 if i == 5 else 255))
                            for i in range(1, 7)]
        table = np.stack(table, 1)
        aindex = (abits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
        alpha = np.take_along_axis(table, aindex.astype(np.int64), 1)
    elif fmt == 15:
        abits = np.zeros(len(blocks), np.uint64)
        for k in range(8):
            abits |= blocks[:, k].astype(np.uint64) << np.uint64(8 * k)
        alpha = (((abits[:, None] >> (np.uint64(4) * np.arange(16, dtype=np.uint64))) & np.uint64(15)) * 17).astype(np.int32)
    pixels = np.concatenate([rgb, alpha[:, :, None]], 2).reshape(hb, wb, 4, 4, 4).transpose(0, 2, 1, 3, 4)
    return pixels.reshape(hb * 4, wb * 4, 4)[:height, :width].astype(np.uint8)


def dxt_encode(image, fmt):
    """RGBA (h, w, 4) to DXT1 (fmt 14) or DXT5 (16) blocks: each block's
    colours between its extremes along their main axis"""
    h, w = image.shape[:2]
    blocks = image.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.float64)
    rgb = blocks[:, :, :3]
    mean = rgb.mean(1, keepdims=True)
    centered = rgb - mean
    cov = np.einsum("nki,nkj->nij", centered, centered)
    axis = np.ones((len(blocks), 3)) / np.sqrt(3)
    for _ in range(4):
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum("nki,ni->nk", centered, axis)
    lo = mean[:, 0] + axis * t.min(1, keepdims=True)
    hi = mean[:, 0] + axis * t.max(1, keepdims=True)
    c0, c1 = _565(np.clip(hi, 0, 255)), _565(np.clip(lo, 0, 255))
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = _rgb(c0).astype(np.float64), _rgb(c1).astype(np.float64)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    distance = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1)
    index = distance.argmin(2).astype(np.uint32)
    index = np.where((c0 == c1)[:, None], 0, index)
    bits = (index << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    color = np.zeros((len(blocks), 8), np.uint8)
    color[:, 0], color[:, 1] = c0 & 255, c0 >> 8
    color[:, 2], color[:, 3] = c1 & 255, c1 >> 8
    for k in range(4):
        color[:, 4 + k] = (bits >> (8 * k)) & 255
    if fmt == 14:
        return color.tobytes()
    alpha = blocks[:, :, 3]
    a0, a1 = alpha.max(1), alpha.min(1)
    table = np.stack([a0, a1] + [(a0 * (7 - i) + a1 * i) / 7 for i in range(1, 7)], 1)
    aindex = np.abs(alpha[:, :, None] - table[:, None, :]).argmin(2).astype(np.uint64)
    aindex = np.where((a0 == a1)[:, None], 0, aindex)
    abits = (aindex << (np.uint64(3) * np.arange(16, dtype=np.uint64))).sum(1)
    out = np.zeros((len(blocks), 16), np.uint8)
    out[:, 0], out[:, 1] = a0.astype(np.uint8), a1.astype(np.uint8)
    for k in range(6):
        out[:, 2 + k] = ((abits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    out[:, 8:] = color
    return out.tobytes()


def power_of_two_levels(image, fmt, levels=6):
    """an RGBA image resized to the power of two at or below its size, and
    its mipmaps (as many as `levels`, none under 4x4), DXT encoded"""
    from PIL import Image
    h, w = image.shape[:2]
    pw, ph = 1 << (max(w, 4).bit_length() - 1), 1 << (max(h, 4).bit_length() - 1)
    picture = Image.fromarray(image, "RGBA")
    data, count = b"", 0
    while count < levels and pw >= 4 and ph >= 4:
        level = np.asarray(picture.resize((pw, ph), Image.LANCZOS if count == 0 else Image.BILINEAR))
        data += dxt_encode(level, fmt)
        count += 1
        pw, ph = pw // 2, ph // 2
    return count, data
