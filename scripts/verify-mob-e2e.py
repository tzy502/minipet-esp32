#!/usr/bin/env python3
"""
verify-mob-e2e.py —— 「怪物资产可用」部署后端到端验收（一条命令）

覆盖链路（每一步都打到真服务器/真设备，不做任何 mock）：
  ① POST /api/admin/devices/{id}/push {kind:"mob", id:<mobId>, switch:true} → 202
  ② 轮询 GET /api/device/manifest?deviceId=… 直到出现该实体的 PARTS + 各动作 LAYOUT
     （并核对契约字段：selector=mob / entity=mob:<id> / defaultAction / origin=[0,0]）
  ③ 经既有 action 指令通道下发切换：POST /command {"type":"action","value":"entity:mob:<id>"}
     （与 Web 推送下发的 `entity` 指令等效；这条通道任何服务端版本都有）
  ④ 触发设备抓帧并接收 UDP 广播（:9999，"MPFB" 协议）→ 落 PNG，人眼复核屏上是不是怪物
  ⑤ 打印 PASS/FAIL 汇总；PNG 路径一并给出

用法：
  python3 scripts/verify-mob-e2e.py --server http://192.168.3.46:38090 \
      --device dev-693ea4 --mob 100100 [--trigger shot] [--out /tmp/mob_e2e.png] [--no-frame]

  --trigger：216 用 `shot`（action 魔数）；185B 用 `::shot`（bubble 魔数）。
             不给则按设备 profile 猜：宽高 ≥480 打 shot，否则 ::shot？——不猜，
             默认 `shot`，185B 请显式传 `--trigger ::shot`。

退出码：0 = 全通；1 = 有步骤失败（逐条打印原因）。
"""
import argparse
import json
import socket
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib

FAIL = []
OK = []


def step(name, ok, detail=""):
    (OK if ok else FAIL).append(name)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" —— {detail}" if detail else ""))
    return ok


def http_json(url, payload=None, method=None, timeout=30):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data else "GET"),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
            return r.status, body
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def recv_frame(duration, out_png):
    """接收设备 UDP 帧倾倒（协议：第 0 包 'MPFB'+w+h+n；其后 seq(u16)+≤1400B 像素）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 9999))
    s.settimeout(2.0)
    hdr, pkts, t0 = None, {}, time.time()
    while time.time() - t0 < duration:
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        if data[:4] == b"MPFB":
            w, h, n = struct.unpack("<HHH", data[4:10])
            hdr, pkts = (w, h, n), {}
        elif hdr:
            pkts[struct.unpack("<H", data[:2])[0]] = data[2:]
    s.close()
    if not hdr:
        return False, "没收到帧（设备没抓帧？触发魔数不对？防火墙？）"
    w, h, n = hdr
    raw = b"".join(pkts.get(i, b"") for i in range(n))[: w * h * 2].ljust(w * h * 2, b"\x00")
    rows = []
    for y in range(h):
        line = bytearray([0])
        for x in range(w):
            v = raw[(y * w + x) * 2] | (raw[(y * w + x) * 2 + 1] << 8)
            line += bytes((((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3))
        rows.append(bytes(line))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
           + chunk(b"IEND", b""))
    open(out_png, "wb").write(png)
    return True, f"{w}x{h}，收到 {len(pkts)}/{n} 包 → {out_png}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True, help="服务端基址，如 http://192.168.3.46:38090")
    ap.add_argument("--device", required=True, help="设备 id（dev-xxxxxx）")
    ap.add_argument("--mob", required=True, help="怪物 id，如 100100")
    ap.add_argument("--trigger", default="shot", help="抓帧魔数：216=shot / 185B=::shot")
    ap.add_argument("--out", default="/tmp/mob_e2e.png")
    ap.add_argument("--wait-dl", type=int, default=120, help="等素材下载并进清单的最长秒数")
    ap.add_argument("--no-frame", action="store_true", help="跳过抓帧步骤")
    a = ap.parse_args()

    entity = f"mob:{a.mob}"
    base = a.server.rstrip("/")

    # ① 推送
    st, body = http_json(f"{base}/api/admin/devices/{a.device}/push",
                         {"kind": "mob", "id": a.mob, "switch": True})
    step("① push kind=mob 受理（202）", st == 202, f"HTTP {st} {body[:160]}")
    if st != 202:
        return finish()

    # ② 等清单出现实体 + 契约字段核对
    deadline, seen = time.time() + a.wait_dl, {}
    while time.time() < deadline:
        st, body = http_json(f"{base}/api/device/manifest?deviceId={a.device}")
        if st == 200:
            try:
                assets = json.loads(body).get("assets", {})
            except json.JSONDecodeError:
                assets = {}
            seen = {h: e for h, e in assets.items() if str(e.get("entity", "")) == entity}
            if any(e.get("kind") == "PARTS" for e in seen.values()) and \
               sum(1 for e in seen.values() if e.get("kind") == "LAYOUT") > 0:
                break
        time.sleep(5)
    parts = [e for e in seen.values() if e.get("kind") == "PARTS"]
    lays = [e for e in seen.values() if e.get("kind") == "LAYOUT"]
    step("② 设备清单出现该实体（1 PARTS + N LAYOUT）", bool(parts) and bool(lays),
         f"PARTS {len(parts)} / LAYOUT {len(lays)}")
    if not parts:
        return finish()
    p = parts[0]
    step("②a PARTS 契约字段", p.get("selector") == "mob" and bool(p.get("defaultAction")),
         f"selector={p.get('selector')} defaultAction={p.get('defaultAction')} bytes={p.get('bytes')}")
    origins = {tuple(e.get("origin") or []): 1 for e in lays}
    step("②b LAYOUT origin 恒 [0,0]（修复后口径）", all(o == (0, 0) for o in origins),
         f"见到 origin={sorted(origins)}")

    # ③ 下发切换（既有 action 通道；等价于服务端 push 时下发的 entity 指令）
    st, body = http_json(f"{base}/api/admin/devices/{a.device}/command",
                         {"type": "action", "value": f"entity:{entity}"})
    step("③ 实体切换指令已入队（202）", st == 202, f"HTTP {st} {body[:120]}")
    time.sleep(6)                       # 给设备一拍时间下载/绑定

    # ④ 抓帧
    if a.no_frame:
        print("[SKIP] ④ 抓帧（--no-frame）")
    else:
        payload = ({"type": "action", "value": a.trigger} if not a.trigger.startswith("::")
                   else {"type": "bubble", "value": a.trigger})
        st, _ = http_json(f"{base}/api/admin/devices/{a.device}/command", payload)
        time.sleep(1)
        ok, detail = recv_frame(28, a.out)
        step("④ 收到设备帧并落 PNG", ok, detail)
        if ok:
            print(f"     → 打开看图：{a.out}（屏上应是怪物 {entity}；"
                  f"若仍是纸娃娃，查设备串口日志里的 `形象切换` 行）")

    return finish()


def finish():
    print("\n===== 结果 =====")
    for n in OK:
        print(f"  PASS  {n}")
    for n in FAIL:
        print(f"  FAIL  {n}")
    print(f"{'PASS（全链路通过）' if not FAIL else 'FAIL（见上）'}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
