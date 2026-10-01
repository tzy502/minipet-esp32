#!/usr/bin/env python3
"""串口 TCP 中继 · 服务端（跑在**没有**串口权限的一侧，即本 DSH session）。

  python3 bridge_server.py [TCP_PORT]

另一端（**有**串口权限的 session / 你的终端）跑 bridge_client.py 接上来；
之后本侧即可 `--port socket://127.0.0.1:<TCP_PORT>` 直接烧录/抓日志。
本服务端把该 socket 当成"裸串口"用（pyserial 的 socket:// 语义）。

线协议（客户端 ↔ 服务端）：
  帧格式 = [tag 1B][len 2B 小端][payload len B]
    tag 'D' = 串口数据（双向都走它）
    tag 'P' = 控制（'O'=串口已开；'E'+文本=打开失败）
  · 服务端把收到的 'D' 直接回灌 socket（表现为"串口来的数据"），
    把写往 socket 的字节（烧录器发出的）打成 'D' 帧发给客户端 → 写进真串口。
  · 同一时刻只服务一个客户端（板子只允许一个持有者，防两会话抢）。
"""
import socket, sys, threading, struct

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 33333
lock = threading.Lock()
state = {"conn": None}


def send_frame(conn, tag: bytes, payload: bytes):
    conn.sendall(tag + struct.pack("<H", len(payload)) + payload)


def reader_from_client(conn):
    """客户端来的帧：'D' → 回灌 socket（= 串口输出）；'P' → 打印状态"""
    buf = b""
    try:
        while True:
            d = conn.recv(65536)
            if not d:
                break
            buf += d
            while len(buf) >= 3:
                tag = buf[0:1]
                n = struct.unpack("<H", buf[1:3])[0]
                if len(buf) < 3 + n:
                    break
                payload, buf = buf[3:3 + n], buf[3 + n:]
                if tag == b"D":
                    conn.sendall(payload)          # 回灌：对 socket:// 就是"串口读到的字节"
                elif tag == b"P":
                    txt = payload[1:].decode("utf-8", "replace") if payload[:1] == b"E" else ""
                    print(("  [client] 串口打开失败: " + txt) if txt else "  [client] 串口已打开", flush=True)
    except OSError:
        pass
    finally:
        with lock:
            state["conn"] = None
        print("[relay] 客户端断开", flush=True)
        try:
            conn.close()
        except OSError:
            pass


def writer_to_client(conn):
    """socket 上被写入的字节（烧录器发出的）→ 'D' 帧发客户端 → 写进真串口"""
    try:
        while True:
            d = conn.recv(65536)
            if not d:
                break
            send_frame(conn, b"D", d)
    except OSError:
        pass


def handle(conn, addr):
    with lock:
        if state["conn"] is not None:
            print(f"[relay] 拒绝第二个连接 {addr}（已有持有者）", flush=True)
            conn.close()
            return
        state["conn"] = conn
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[relay] 客户端接入 {addr}", flush=True)
    threading.Thread(target=writer_to_client, args=(conn,), daemon=True).start()
    reader_from_client(conn)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(4)
    print(f"[relay] 监听 127.0.0.1:{PORT} —— 等 bridge_client.py 接入", flush=True)
    while True:
        c, a = srv.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()


if __name__ == "__main__":
    main()
