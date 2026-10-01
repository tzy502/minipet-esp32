#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
整图 BGMAP「分块(tiled) vs 逐行(legacy)」离线对拍 + 读放大静态统计。

契约：docs/ai/map-tiled-format-contract.md（冻结 v1）
  §6 取值算法（唯一正确算法）：tx = x/TILE; ty = y/TILE; in_x = x%TILE; in_y = y%TILE;
      tile_off = px_off + (ty*gx + tx)*TILE*TILE*2;  byte_off = tile_off + (in_y*TILE + in_x)*2
      掩码：cov_off + (ty*gx+tx)*2048 + in_y*16 + (in_x>>3)，位序 MSB-first

本脚本只读包、不写包，做两件事：
  1) --verify（默认）：同一张图的 tiled 包与 legacy 包逐像素对拍
     · 整幅图（static 全图 + tile 全图 + tile 掩码全图，逐像素/逐位）
     · 随机 N 个窗口用 §6 的**字面**取值算法再算一遍（默认 2000 个窗口）
     · 头部字段（map_id/vw/vh/层偏移/strip 描述）与地面表逐字节
     · 条带小 PARTS 包（tiled 记录 vs 逐行记录）整幅像素 + 掩码 + 随机点
  2) --stats（默认）：一个 288×288 世界窗口的读放大静态统计
     · tiled  = 整块 32KB 连续 pread（像素）/ 2KB（掩码）
     · legacy = 跨行距逐行读，按固件 bg_read_rect_rgb 的 64KB 预读块路径建模
                （掩码按 mpak_bgmap_read_tile_mask_rect 的"每行一次 pread"建模）

用法：
  python3 Server/tools/bgmap-tiled-verify.py --tiled <dir|bgmap.mpak> --legacy <dir|bgmap.mpak>
  python3 Server/tools/bgmap-tiled-verify.py --tiled <dir> --legacy <dir> --stats-only
  python3 Server/tools/bgmap-tiled-verify.py --tiled <a.mpak> --legacy <b.mpak> --samples 2000 --window 24

<dir> = 导出目录（含 manifest-assets.json，自动挑 BGMAP 条目；条带小包从同目录解析）；
     也可直接给 BGMAP 的 .mpak 文件。
"""

import argparse
import json
import os
import random
import struct
import sys
import time

# ── 契约常量（docs/ai/map-tiled-format-contract.md §2/§3）──
TILE = 128
TILE_PX_BYTES = TILE * TILE * 2          # 32768
TILE_MASK_BYTES = TILE * TILE // 8       # 2048
MASK_ROW_BYTES = TILE // 8               # 16

MPAK_KIND_BGMAP = 3
MPAK_KIND_PARTS = 1
EXT_MAGIC = 0x4D504745                   # 文件字节序 = 45 47 50 4D
FLAG_FULL_MAP = 0x1
FLAG_TILED = 0x2

# 固件侧读放大模型参数（Firmware/board216/main/render/mpak.c）
RA_CAP = 64 * 1024                       # BG_RA_CAP_DEFAULT：整图窗口读的预读块
SEQ_KBPS_TILED = 1336.0                  # 契约 §8：SD 顺序读实测
SEQ_KBPS_ROWS = 130.0                    # 契约 §8：整图窗口填充（跨行距）实测

BIT_EXPAND = [bytes(((b >> k) & 1) for k in (7, 6, 5, 4, 3, 2, 1, 0)) for b in range(256)]


def align4(v):
    return (v + 3) & ~3


def grid_x(lw):
    return 0 if lw <= 0 else (lw + TILE - 1) // TILE


def grid_y(lh):
    return 0 if lh <= 0 else (lh + TILE - 1) // TILE


def human(n):
    if n >= 1024 * 1024:
        return '%.2f MB' % (n / 1024.0 / 1024.0)
    if n >= 1024:
        return '%.1f KB' % (n / 1024.0)
    return '%d B' % n


class Mpak(object):
    """MPAK 信封（算法规格 §二）：40B 头 + payload + 8B 尾。"""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.raw = f.read()
        self.path = path
        if self.raw[0:4] != b'MPAK':
            raise ValueError('%s: 不是 MPAK 包（magic=%r）' % (path, self.raw[0:4]))
        self.version, self.flags = struct.unpack_from('<HH', self.raw, 4)
        self.kind = struct.unpack_from('<Q', self.raw, 16)[0]
        self.hash = struct.unpack_from('<Q', self.raw, 24)[0]
        self.payload_len = struct.unpack_from('<I', self.raw, 32)[0]
        self.payload = self.raw[40:40 + self.payload_len]
        if len(self.payload) != self.payload_len:
            raise ValueError('%s: payload 截断（声明 %d，实到 %d）'
                             % (path, self.payload_len, len(self.payload)))


class Strip(object):
    __slots__ = ('part_ref', 'y', 'speed_x', 'rx', 'blend')

    def __init__(self, part_ref, y, speed_x, rx, blend):
        self.part_ref = part_ref
        self.y = y
        self.speed_x = speed_x
        self.rx = rx
        self.blend = blend

    def key(self):
        return (self.y, self.speed_x, self.rx, self.blend)


class Bgmap(object):
    """kind=3 BGMAP payload 解析（信封/头/尺寸/地面表/条带语义与旧格式完全一致）。"""

    def __init__(self, path):
        mp = Mpak(path)
        if mp.kind != MPAK_KIND_BGMAP:
            raise ValueError('%s: kind=%d 不是 BGMAP(3)' % (path, mp.kind))
        self.mpak = mp
        p = mp.payload
        self.map_id = p[0:32].split(b'\x00')[0].decode('utf-8', 'replace')
        self.vw, self.vh = struct.unpack_from('<HH', p, 32)
        self.static_len, self.static_off = struct.unpack_from('<II', p, 36)
        self.tile_len, self.tile_off = struct.unpack_from('<II', p, 44)
        self.strip_count = struct.unpack_from('<I', p, 52)[0]
        self.strips = []
        for i in range(self.strip_count):
            o = 56 + 14 * i
            ref, y, sx, rx, bl = struct.unpack_from('<QhhBB', p, o)
            self.strips.append(Strip(ref, y, sx, rx, bl))
        self.strips_end = align4(56 + 14 * self.strip_count)
        # 尾扩展块：ext_off = align4(tile_off + tile_len)
        self.ext_off = align4(self.tile_off + self.tile_len)
        self.flags = 0
        self.tiled = False
        self.full_map = False
        self.ground_off = 0
        self.ground = b''
        self.has_ext = False
        if self.ext_off + 16 <= len(p):
            magic, glen, goff, eflags = struct.unpack_from('<IIII', p, self.ext_off)
            if magic == EXT_MAGIC:
                self.has_ext = True
                self.flags = eflags
                self.tiled = bool(eflags & FLAG_TILED)
                self.full_map = bool(eflags & FLAG_FULL_MAP)
                self.ground_off = goff
                self.ground = p[goff:goff + glen]
        self.gx = grid_x(self.vw)
        self.gy = grid_y(self.vh)
        # 逐行口径的源行距（与固件 bg_static_stride/bg_tile_stride 同口径）
        self.static_stride = (self.vw * 2 if self.static_len == self.vw * self.vh * 2
                              else align4(self.vw * 2))
        self.tile_stride = align4(self.vw * 2)
        self.tile_rgb_len = self.vh * self.tile_stride
        self.tile_mask_off = self.tile_off + self.tile_rgb_len    # 逐行口径掩码起点
        self.tiled_mask_off = self.tile_off + self.gx * self.gy * TILE_PX_BYTES

    # ── 契约 §6：字面取值算法（tiled）──
    def px_at_tiled(self, layer_off, x, y):
        tx, in_x = divmod(x, TILE)
        ty, in_y = divmod(y, TILE)
        off = layer_off + (ty * self.gx + tx) * TILE_PX_BYTES + (in_y * TILE + in_x) * 2
        return self.mpak.payload[off:off + 2]

    def mask_at_tiled(self, cov_off, x, y):
        tx, in_x = divmod(x, TILE)
        ty, in_y = divmod(y, TILE)
        off = cov_off + (ty * self.gx + tx) * TILE_MASK_BYTES + in_y * MASK_ROW_BYTES + (in_x >> 3)
        return (self.mpak.payload[off] >> (7 - (in_x & 7))) & 1

    # ── 逐行口径取值 ──
    def px_at_rows(self, layer_off, stride, x, y):
        off = layer_off + y * stride + x * 2
        return self.mpak.payload[off:off + 2]

    def mask_at_rows(self, x, y):
        bit = y * self.vw + x
        off = self.tile_mask_off + (bit >> 3)
        return (self.mpak.payload[off] >> (7 - (bit & 7))) & 1

    def static_px(self, x, y):
        if self.tiled:
            return self.px_at_tiled(self.static_off, x, y)
        return self.px_at_rows(self.static_off, self.static_stride, x, y)

    def tile_px(self, x, y):
        if self.tiled:
            return self.px_at_tiled(self.tile_off, x, y)
        return self.px_at_rows(self.tile_off, self.tile_stride, x, y)

    def tile_mask(self, x, y):
        if self.tiled:
            return self.mask_at_tiled(self.tiled_mask_off, x, y)
        return self.mask_at_rows(x, y)

    # ── 全图展开（与逐像素口径等价，速度友好）──
    def expand_pixels(self, layer_off, lw, lh, tiled):
        return _expand_pixels(self.mpak.payload, layer_off, lw, lh, tiled)

    def expand_mask(self, cov_off, lw, lh, tiled):
        return _expand_mask(self.mpak.payload, cov_off, lw, lh, tiled)


def _expand_pixels(p, layer_off, lw, lh, tiled):
    """返回 lw*lh 的 RGB565 行主序字节（每像素 2B 小端）；两种布局展开成同一形态便于比对。"""
    out = bytearray(lw * 2 * lh)
    if not tiled:
        stride = align4(lw * 2)
        for y in range(lh):
            s = layer_off + y * stride
            out[y * lw * 2:(y + 1) * lw * 2] = p[s:s + lw * 2]
        return out
    gx = grid_x(lw)
    for y in range(lh):
        ty, in_y = divmod(y, TILE)
        row = y * lw * 2
        for tx in range(gx):
            n = min(TILE, lw - tx * TILE)
            s = layer_off + (ty * gx + tx) * TILE_PX_BYTES + in_y * TILE * 2
            out[row + tx * TILE * 2:row + tx * TILE * 2 + n * 2] = p[s:s + n * 2]
    return out


def _expand_mask(p, cov_off, lw, lh, tiled):
    """返回 lw*lh 的 0/1 位（每像素 1B），便于直接比对。"""
    out = bytearray(lw * lh)
    if not tiled:
        for y in range(lh):
            bit0 = y * lw
            fb = bit0 >> 3
            nbytes = ((bit0 + lw - 1) >> 3) - fb + 1
            seg = p[cov_off + fb:cov_off + fb + nbytes]
            expanded = b''.join(BIT_EXPAND[b] for b in seg)
            shift = bit0 & 7
            out[y * lw:(y + 1) * lw] = expanded[shift:shift + lw]
        return out
    gx = grid_x(lw)
    for y in range(lh):
        ty, in_y = divmod(y, TILE)
        row = y * lw
        for tx in range(gx):
            n = min(TILE, lw - tx * TILE)
            nb = (n + 7) // 8
            s = cov_off + (ty * gx + tx) * TILE_MASK_BYTES + in_y * MASK_ROW_BYTES
            expanded = b''.join(BIT_EXPAND[b] for b in p[s:s + nb])
            out[row + tx * TILE:row + tx * TILE + n] = expanded[:n]
    return out


class Parts(object):
    """kind=1 PARTS payload（索引 20B/条；位图记录按 tiled 与否两种口径）。"""

    def __init__(self, path):
        mp = Mpak(path)
        if mp.kind != MPAK_KIND_PARTS:
            raise ValueError('%s: kind=%d 不是 PARTS(1)' % (path, mp.kind))
        self.mpak = mp
        p = mp.payload
        n = struct.unpack_from('<I', p, 0)[0]
        self.count = n
        self.bmp_base = 4 + 20 * n
        self.entries = []
        for i in range(n):
            o = 4 + 20 * i
            pid, grp, w, h, ox, oy, off, _pad = struct.unpack_from('<IHHHhhIH', p, o)
            self.entries.append(dict(part_id=pid, expr_group=grp, w=w, h=h,
                                     origin_x=ox, origin_y=oy, offset=off))

    def _base(self, i):
        e = self.entries[i]
        return self.bmp_base + e['offset']

    def px(self, i, x, y, tiled):
        e = self.entries[i]
        base = self._base(i)
        p = self.mpak.payload
        if tiled:
            gx = grid_x(e['w'])
            tx, in_x = divmod(x, TILE)
            ty, in_y = divmod(y, TILE)
            off = base + (ty * gx + tx) * TILE_PX_BYTES + (in_y * TILE + in_x) * 2
        else:
            off = base + y * align4(e['w'] * 2) + x * 2
        return p[off:off + 2]

    def mask(self, i, x, y, tiled):
        e = self.entries[i]
        base = self._base(i)
        p = self.mpak.payload
        if tiled:
            gx = grid_x(e['w'])
            cov = base + gx * grid_y(e['h']) * TILE_PX_BYTES
            tx, in_x = divmod(x, TILE)
            ty, in_y = divmod(y, TILE)
            off = cov + (ty * gx + tx) * TILE_MASK_BYTES + in_y * MASK_ROW_BYTES + (in_x >> 3)
            return (p[off] >> (7 - (in_x & 7))) & 1
        bit = y * e['w'] + x
        off = base + align4(e['w'] * 2) * e['h'] + (bit >> 3)
        return (p[off] >> (7 - (bit & 7))) & 1

    def record_len(self, i, tiled):
        e = self.entries[i]
        if tiled:
            return grid_x(e['w']) * grid_y(e['h']) * (TILE_PX_BYTES + TILE_MASK_BYTES)
        return align4(e['w'] * 2) * e['h'] + align4((e['w'] * e['h'] + 7) // 8)

    def expand_pixels(self, i, tiled):
        e = self.entries[i]
        return _expand_pixels(self.mpak.payload, self._base(i), e['w'], e['h'], tiled)

    def expand_mask(self, i, tiled):
        e = self.entries[i]
        cov = self._base(i) + (grid_x(e['w']) * grid_y(e['h']) * TILE_PX_BYTES if tiled
                               else align4(e['w'] * 2) * e['h'])
        return _expand_mask(self.mpak.payload, cov, e['w'], e['h'], tiled)


# ══════════════════════════════════════════════════════════════
# 目录 → BGMAP 解析
# ══════════════════════════════════════════════════════════════

def resolve_bgmap(arg):
    """arg = 导出目录（自动从 manifest-assets.json 挑 BGMAP）或 .mpak 文件。"""
    if os.path.isdir(arg):
        man = os.path.join(arg, 'manifest-assets.json')
        if not os.path.isfile(man):
            raise SystemExit('%s: 目录里没有 manifest-assets.json（请直接给 BGMAP 的 .mpak 文件）' % arg)
        with open(man, 'r', encoding='utf-8') as f:
            root = json.load(f)
        cands = [(h, e) for h, e in (root.get('assets') or {}).items()
                 if str(e.get('kind', '')).upper() == 'BGMAP']
        if not cands:
            raise SystemExit('%s: manifest 里没有 BGMAP 条目' % arg)
        if len(cands) > 1:
            print('[warn] %s: manifest 有 %d 个 BGMAP 条目，取第一个（%s）'
                  % (arg, len(cands), cands[0][0]), file=sys.stderr)
        h = cands[0][0]
        path = os.path.join(arg, '%s.mpak' % h)
        if not os.path.isfile(path):
            alt = sorted(f for f in os.listdir(arg) if f.startswith(h))
            if not alt:
                raise SystemExit('%s: 找不到 %s.mpak' % (arg, h))
            path = os.path.join(arg, alt[0])
        return path, arg
    return arg, os.path.dirname(os.path.abspath(arg))


def strip_path(asset_dir, part_ref):
    name = '%016x.mpak' % part_ref
    p = os.path.join(asset_dir, name)
    if os.path.isfile(p):
        return p
    for f in sorted(os.listdir(asset_dir)):
        if f.lower().startswith('%016x' % part_ref):
            return os.path.join(asset_dir, f)
    return None


# ══════════════════════════════════════════════════════════════
# 1) 等价性对拍
# ══════════════════════════════════════════════════════════════

def verify(tiled_path, tiled_dir, rows_path, rows_dir, samples, win, seed):
    t0 = time.time()
    a = Bgmap(tiled_path)     # tiled
    b = Bgmap(rows_path)      # legacy rows
    fails = []

    def check(cond, msg):
        print('  %s %s' % ('[OK]  ' if cond else '[FAIL]', msg))
        if not cond:
            fails.append(msg)

    print('=' * 78)
    print('包 A（tiled）: %s' % tiled_path)
    print('  flags=0x%x  tiled=%s full_map=%s  vw×vh=%d×%d  瓦片 gx×gy=%d×%d=%d 块'
          % (a.flags, a.tiled, a.full_map, a.vw, a.vh, a.gx, a.gy, a.gx * a.gy))
    print('  static %d B @%d   tile %d B @%d   strips=%d   ground=%d B'
          % (a.static_len, a.static_off, a.tile_len, a.tile_off, a.strip_count, len(a.ground)))
    print('包 B（legacy）: %s' % rows_path)
    print('  flags=0x%x  tiled=%s full_map=%s  static %d B  tile %d B  strips=%d  ground=%d B'
          % (b.flags, b.tiled, b.full_map, b.static_len, b.tile_len, b.strip_count, len(b.ground)))
    print('-' * 78)

    check(a.tiled and not b.tiled,
          'flags：A 置 bit1(TILED)=%s，B 未置 bit1=%s' % (a.tiled, not b.tiled))
    check(a.full_map and b.full_map, 'flags：两包 bit0(full_map) 均为 1')
    exp_static_tiled = a.gx * a.gy * TILE_PX_BYTES
    exp_tile_tiled = a.gx * a.gy * (TILE_PX_BYTES + TILE_MASK_BYTES)
    check(a.static_len == exp_static_tiled,
          'A static_len=%d == gx*gy*32768=%d' % (a.static_len, exp_static_tiled))
    check(a.tile_len == exp_tile_tiled,
          'A tile_len=%d == gx*gy*(32768+2048)=%d（掩码紧跟像素区、无额外补齐）'
          % (a.tile_len, exp_tile_tiled))
    exp_static_rows = a.vh * align4(a.vw * 2)
    exp_tile_rows = a.vh * align4(a.vw * 2) + align4((a.vw * a.vh + 7) // 8)
    check(b.static_len == exp_static_rows,
          'B static_len=%d == vh*align4(vw*2)=%d' % (b.static_len, exp_static_rows))
    check(b.tile_len == exp_tile_rows,
          'B tile_len=%d == 行区+align4(掩码)=%d' % (b.tile_len, exp_tile_rows))

    check(a.map_id == b.map_id, 'map_id 一致：%r' % a.map_id)
    check((a.vw, a.vh) == (b.vw, b.vh), 'vw×vh 一致：%d×%d' % (a.vw, a.vh))
    check((a.static_off, b.static_off) == (a.strips_end, b.strips_end),
          '层偏移起始（信封/头口径不变）一致：static@%d == align4(56+14×%d)' % (a.static_off, a.strip_count))
    check(a.tile_off == a.static_off + a.static_len and b.tile_off == b.static_off + b.static_len,
          'tile 紧跟 static：A %d=%d+%d  B %d=%d+%d（两包 static 长度按各自布局不同，'
          '故 tile 绝对偏移不同属预期）'
          % (a.tile_off, a.static_off, a.static_len, b.tile_off, b.static_off, b.static_len))
    check(a.strip_count == b.strip_count, 'strip_count 一致：%d' % a.strip_count)
    check([s.key() for s in a.strips] == [s.key() for s in b.strips],
          '条带描述（y/speed_x/rx/blend）逐条一致：%s'
          % ', '.join('y=%d sp=%d rx=%d bl=%d' % s.key() for s in a.strips))
    check(a.ground == b.ground and len(a.ground) == a.vw * 2,
          '地面表逐字节一致（%d 列 × u16 = %d B）' % (a.vw, len(a.ground)))

    print('  展开整幅图（%d×%d = %.2f Mpx）…' % (a.vw, a.vh, a.vw * a.vh / 1e6))
    t1 = time.time()
    sa = a.expand_pixels(a.static_off, a.vw, a.vh, True)
    sb = b.expand_pixels(b.static_off, b.vw, b.vh, False)
    ok = (sa == sb)
    check(ok, 'static 整幅图逐像素一致（%d px × RGB565）' % (a.vw * a.vh))
    if not ok:
        for i in range(0, len(sa), 2):
            if sa[i:i + 2] != sb[i:i + 2]:
                print('        首个差异 @px(%d,%d)：tiled=%s rows=%s'
                      % ((i // 2) % a.vw, (i // 2) // a.vw, sa[i:i + 2].hex(), sb[i:i + 2].hex()))
                break
    ta = a.expand_pixels(a.tile_off, a.vw, a.vh, True)
    tb = b.expand_pixels(b.tile_off, b.vw, b.vh, False)
    ok = (ta == tb)
    check(ok, 'tile 整幅图逐像素一致（%d px × RGB565）' % (a.vw * a.vh))
    if not ok:
        for i in range(0, len(ta), 2):
            if ta[i:i + 2] != tb[i:i + 2]:
                print('        首个差异 @px(%d,%d)：tiled=%s rows=%s'
                      % ((i // 2) % a.vw, (i // 2) // a.vw, ta[i:i + 2].hex(), tb[i:i + 2].hex()))
                break
    ma = a.expand_mask(a.tiled_mask_off, a.vw, a.vh, True)
    mb = b.expand_mask(b.tile_mask_off, b.vw, b.vh, False)
    ok = (ma == mb)
    check(ok, 'tile 掩码整幅图逐位一致（%d bit，其中置 1 共 %d 位）' % (len(ma), sum(ma)))
    if not ok:
        for i in range(len(ma)):
            if ma[i] != mb[i]:
                print('        首个差异 @px(%d,%d)：tiled=%d rows=%d' % (i % a.vw, i // a.vw, ma[i], mb[i]))
                break
    print('  整幅展开耗时 %.2fs' % (time.time() - t1))

    if samples > 0 and a.vw > win and a.vh > win:
        rnd = random.Random(seed)
        t2 = time.time()
        diff = 0
        first = None
        for _ in range(samples):
            x0 = rnd.randrange(0, a.vw - win + 1)
            y0 = rnd.randrange(0, a.vh - win + 1)
            for yy in range(y0, y0 + win):
                for xx in range(x0, x0 + win):
                    if a.static_px(xx, yy) != b.static_px(xx, yy) or \
                       a.tile_px(xx, yy) != b.tile_px(xx, yy) or \
                       a.tile_mask(xx, yy) != b.tile_mask(xx, yy):
                        diff += 1
                        if first is None:
                            first = (xx, yy)
        check(diff == 0,
              '随机 %d 个 %d×%d 窗口 × 契约 §6 字面取值算法（static+tile+掩码共 %d 次取值）0 差异%s'
              % (samples, win, win, samples * win * win,
                 '' if first is None else '，首个差异 @%s' % (first,)))
        print('  随机窗口对拍耗时 %.2fs' % (time.time() - t2))

    for i in range(min(len(a.strips), len(b.strips))):
        pa = strip_path(tiled_dir, a.strips[i].part_ref)
        pb = strip_path(rows_dir, b.strips[i].part_ref)
        if pa is None or pb is None:
            check(False, '条带 %d 包缺失（tiled=%s legacy=%s）' % (i, pa, pb))
            continue
        A = Parts(pa)
        B = Parts(pb)
        ea, eb = A.entries[0], B.entries[0]
        check((ea['w'], ea['h'], ea['origin_x'], ea['origin_y']) ==
              (eb['w'], eb['h'], eb['origin_x'], eb['origin_y']),
              '条带 %d：索引一致 %dx%d origin=(%d,%d)（tiled 记录 %d B / 逐行记录 %d B）'
              % (i, ea['w'], ea['h'], ea['origin_x'], ea['origin_y'],
                 A.record_len(0, True), B.record_len(0, False)))
        check(A.expand_pixels(0, True) == B.expand_pixels(0, False),
              '条带 %d：整幅像素逐像素一致（%d×%d）' % (i, ea['w'], ea['h']))
        mka = A.expand_mask(0, True)
        mkb = B.expand_mask(0, False)
        check(mka == mkb, '条带 %d：掩码逐位一致（置 1 共 %d 位）' % (i, sum(mka)))
        rnd2 = random.Random(seed + 7 + i)
        bad = 0
        for _ in range(200):
            xx = rnd2.randrange(0, ea['w'])
            yy = rnd2.randrange(0, ea['h'])
            if A.px(0, xx, yy, True) != B.px(0, xx, yy, False) or \
               A.mask(0, xx, yy, True) != B.mask(0, xx, yy, False):
                bad += 1
        check(bad == 0, '条带 %d：200 个随机点 §6 字面取值 0 差异' % i)

    print('-' * 78)
    print('等价性对拍：%s（耗时 %.1fs）'
          % ('✔ 全部通过，0 差异' if not fails else '✘ %d 项失败' % len(fails), time.time() - t0))
    print('=' * 78)
    return 1 if fails else 0


# ══════════════════════════════════════════════════════════════
# 2) 读放大静态统计
# ══════════════════════════════════════════════════════════════

def tiled_reads(x0, y0, w, h, tile_bytes):
    """tiled：整块读（每块一次连续 pread）。返回 (块数, 字节)。"""
    if w <= 0 or h <= 0:
        return 0, 0
    tx0, ty0 = x0 // TILE, y0 // TILE
    tx1, ty1 = (x0 + w - 1) // TILE, (y0 + h - 1) // TILE
    n = (tx1 - tx0 + 1) * (ty1 - ty0 + 1)
    return n, n * tile_bytes


def rows_reads(x0, y0, w, h, stride, layer_len, ra_cap=RA_CAP):
    """legacy 像素：固件 bg_read_rect_rgb 慢路径（64KB 预读块覆盖整行距）。
    返回 (读次数, 搬运字节, 有用字节, 无缓冲退化时的逐行读次数)。"""
    row_bytes = w * 2
    col_off = x0 * 2
    reads = moved = 0
    r = y0
    while r < y0 + h:
        base = r * stride
        span = min(ra_cap, (y0 + h - r) * stride)
        span = max(span, row_bytes + col_off)
        if layer_len > base:
            span = min(span, layer_len - base)
        else:
            break
        reads += 1
        moved += span
        nrows = 0
        while r + nrows < y0 + h and nrows * stride + col_off + row_bytes <= span:
            nrows += 1
        r += nrows if nrows > 0 else 1
    return reads, moved, row_bytes * h, h


def mask_rows_reads(x0, y0, w, h):
    """legacy tile 掩码：固件每行一次 pread（含跨行卷绕的多读 1 字节）。"""
    nbytes = ((x0 + w - 1) >> 3) - (x0 >> 3) + 1
    return h, h * nbytes, ((w + 7) // 8) * h, h


def strip_reads(strips_meta, x0, y0, w, h):
    """条带：按各自 w×h 独立分块（tiled）vs 逐行。
    strips_meta = [(sw, sh, y)]（来自条带 PARTS 索引 + BGMAP strip.y）。
    可见矩形 = 窗口 ∩ 条带竖直区间；列数保守取 min(w, sw)（滚动相位无关，静态统计口径）。"""
    t_r = t_b = l_r = l_b = 0
    for (sw, sh, sy) in strips_meta:
        yy0 = max(y0, sy)
        yy1 = min(y0 + h, sy + sh)
        if yy1 <= yy0:
            continue
        rows = yy1 - yy0
        cols = min(w, sw)
        n, nbytes = tiled_reads(0, 0, cols, rows, TILE_PX_BYTES)
        t_r += n * 2                    # 像素块 + 掩码块
        t_b += nbytes + n * TILE_MASK_BYTES
        r, moved, _u, _d = rows_reads(0, 0, cols, rows, align4(sw * 2),
                                      align4(sw * 2) * sh)
        l_r += r * 2                    # 像素行读 + 掩码行读（每行一次）
        l_b += moved + rows * (((cols + 7) // 8) + 1)
    return t_r, t_b, l_r, l_b


def stats(tiled_path, rows_path, tiled_dir, win, positions):
    a = Bgmap(tiled_path)
    b = Bgmap(rows_path)
    # 条带源尺寸（条带像素在独立小 PARTS 包内）→ 用于条带层统计
    meta = []
    for s in a.strips:
        p = strip_path(tiled_dir, s.part_ref)
        if p is None:
            continue
        try:
            pt = Parts(p)
            e = pt.entries[0]
            meta.append((e['w'], e['h'], s.y))
        except Exception as ex:
            print('  [warn] 条带包 %s 解析失败: %s' % (p, ex), file=sys.stderr)

    print('=' * 78)
    print('读放大静态统计：窗口 %d×%d 世界 px' % (win, win))
    print('  模型：tiled = 每次一整块连续 pread（像素 32KB / 掩码 2KB，可跑满 SD 顺序吞吐）；')
    print('        legacy 像素 = 固件 bg_read_rect_rgb 慢路径（%dKB 预读块跨行距读），' % (RA_CAP // 1024))
    print('        legacy 掩码 = 固件 mpak_bgmap_read_tile_mask_rect（每行一次 pread）。')
    print('  包：tiled vw×vh=%d×%d 瓦片 %d×%d=%d 块；legacy static stride=%d B / tile stride=%d B'
          % (a.vw, a.vh, a.gx, a.gy, a.gx * a.gy, b.static_stride, b.tile_stride))
    print('  条带 %d 条（源尺寸 %s）'
          % (len(meta), ', '.join('%dx%d@y%d' % m for m in meta) if meta else '无'))
    print('-' * 78)
    fmt = '  %-20s %22s | %24s | %8s'
    print(fmt % ('窗口位置', 'tiled（整块读）', 'legacy（跨行距逐行）', '搬运比'))
    print('  ' + '-' * 88)
    worst = 0.0
    worst_row = None
    for (x0, y0) in positions:
        x0 = max(0, min(x0, a.vw - win))
        y0 = max(0, min(y0, a.vh - win))
        w = min(win, a.vw - x0)
        h = min(win, a.vh - y0)
        st_n, st_b = tiled_reads(x0, y0, w, h, TILE_PX_BYTES)
        tl_n, tl_b = tiled_reads(x0, y0, w, h, TILE_PX_BYTES)
        mk_n, mk_b = tiled_reads(x0, y0, w, h, TILE_MASK_BYTES)
        sp_tr, sp_tb, sp_lr, sp_lb = strip_reads(meta, x0, y0, w, h)
        t_reads = st_n + tl_n + mk_n + sp_tr
        t_bytes = st_b + tl_b + mk_b + sp_tb
        st_r, st_rb, st_u, st_d = rows_reads(x0, y0, w, h, b.static_stride, b.static_len)
        tl_r, tl_rb, tl_u, tl_d = rows_reads(x0, y0, w, h, b.tile_stride, b.tile_rgb_len)
        mk_r, mk_rb, mk_u, mk_d = mask_rows_reads(x0, y0, w, h)
        l_reads = st_r + tl_r + mk_r + sp_lr
        l_bytes = st_rb + tl_rb + mk_rb + sp_lb
        ratio = t_bytes / float(l_bytes) if l_bytes else 0.0
        print(fmt % ('(%d,%d) %d×%d' % (x0, y0, w, h),
                     '%d 次 / %s%s' % (t_reads, human(t_bytes), ' (+条带)' if sp_tr else ''),
                     '%d 次 / %s' % (l_reads, human(l_bytes)),
                     '%.3f' % ratio))
        if ratio > worst:
            worst = ratio
            worst_row = (x0, y0, w, h, t_reads, t_bytes, l_reads, l_bytes,
                         st_n + tl_n, st_b + tl_b, mk_n, mk_b, sp_tr, sp_tb, sp_lr, sp_lb,
                         st_r + tl_r, st_rb + tl_rb, st_u + tl_u, mk_r, mk_rb, mk_u, st_d + tl_d)
    print('  ' + '-' * 88)
    (x0, y0, w, h, t_reads, t_bytes, l_reads, l_bytes, pxblocks, pxbytes, mk_n, mk_b,
     sp_tr, sp_tb, sp_lr, sp_lb, pxr, pxrb, pxu, mkr, mkrb, mku, deg) = worst_row
    print('最差窗口 (%d,%d) %d×%d：' % (x0, y0, w, h))
    print('  static+tile 像素   tiled %3d 次整块读 / %-9s   legacy %4d 次读 / %-9s（有用 %s）'
          % (pxblocks, human(pxbytes), pxr, human(pxrb), human(pxu)))
    print('  tile 掩码         tiled %3d 次整块读 / %-9s   legacy %4d 次逐行读 / %-9s（有用 %s）'
          % (mk_n, human(mk_b), mkr, human(mkrb), human(mku)))
    if sp_tr or sp_lr:
        print('  条带（%d 条合计） tiled %3d 次整块读 / %-9s   legacy %4d 次读 / %-9s'
              % (len(meta), sp_tr, human(sp_tb), sp_lr, human(sp_lb)))
    print('  ── 合计           tiled %3d 次连续读 / %-9s   legacy %4d 次读 / %-9s'
          % (t_reads, human(t_bytes), l_reads, human(l_bytes)))
    print('  搬运字节比 tiled/legacy = %.3f（契约 §7.2 要求 ≤ 1.2）' % (t_bytes / float(l_bytes)))
    print('  估算耗时：tiled %.0f ms（%.0f KB @ %.0f KB/s 顺序）；'
          'legacy %.1f s（%.0f KB @ %.0f KB/s 实测跨行距口径）'
          % (t_bytes / 1024.0 / SEQ_KBPS_TILED * 1000.0, t_bytes / 1024.0, SEQ_KBPS_TILED,
             l_bytes / 1024.0 / SEQ_KBPS_ROWS, l_bytes / 1024.0, SEQ_KBPS_ROWS))
    print('  参考：无预读缓冲退化路径（逐行）= %d 次读（tiled 仍是 %d 次）' % (deg, t_reads))
    print('=' * 78)


def main():
    ap = argparse.ArgumentParser(description='整图 BGMAP 分块 vs 逐行 对拍 + 读放大统计')
    ap.add_argument('--tiled', required=True, help='tiled 包（导出目录 或 BGMAP .mpak）')
    ap.add_argument('--legacy', required=True, help='legacy（逐行）包（导出目录 或 BGMAP .mpak）')
    ap.add_argument('--samples', type=int, default=2000, help='随机窗口数（默认 2000；0 = 跳过）')
    ap.add_argument('--window', type=int, default=24, help='随机窗口边长（默认 24）')
    ap.add_argument('--win', type=int, default=288, help='读放大统计的窗口边长（默认 288）')
    ap.add_argument('--seed', type=int, default=20261001)
    ap.add_argument('--stats-only', action='store_true', help='只跑读放大统计')
    ap.add_argument('--verify-only', action='store_true', help='只跑等价性对拍')
    args = ap.parse_args()

    tiled_path, tiled_dir = resolve_bgmap(args.tiled)
    rows_path, _rows_dir = resolve_bgmap(args.legacy)
    if not os.path.isfile(tiled_path):
        raise SystemExit('找不到 tiled 包：%s' % tiled_path)
    if not os.path.isfile(rows_path):
        raise SystemExit('找不到 legacy 包：%s' % rows_path)

    rc = 0
    if not args.stats_only:
        rc = verify(tiled_path, tiled_dir, rows_path, _rows_dir, args.samples, args.window, args.seed)
    if not args.verify_only:
        a = Bgmap(tiled_path)
        cx, cy = a.vw // 2, a.vh // 2
        positions = [(0, 0), (a.vw - args.win, 0), (0, a.vh - args.win),
                     (a.vw - args.win, a.vh - args.win), (cx, cy),
                     (cx - args.win // 2, cy - args.win // 2),
                     (a.vw // 4, a.vh // 4), (3 * a.vw // 4, 3 * a.vh // 4)]
        stats(tiled_path, rows_path, tiled_dir, args.win, positions)
    return rc


if __name__ == '__main__':
    sys.exit(main())
