#!/usr/bin/env python3
"""串口 TCP 中继 · 客户端（跑在**有**串口权限的一侧）。

  python3 bridge_client.py /dev/cu.usbmodem21101 [TCP_PORT]

它把真实串口桥到 127.0.0.1:<TCP_PORT>（bridge_server 那一侧）。
跑起来后保持前台即可（Ctrl-C 断开）。**只有这一个进程碰串口**，
别同时开 idf_monitor / capture_boot，否则抢口。
"""
import socket, sys, threading, struct, time

port_name = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem21101"
tcp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 33333

import serial  # pyserial（IDF 自带 python 有）


def send_frame(sock, tag: bytes, payload: bytes):
    sock.sendall(tag + struct.pack("<H", len(payload)) + payload)


def main():
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.05)
    except Exception as e:
        print(f"[bridge] 打不开串口 {port_name}: {e}")
        raise SystemExit(1)
    ser.dtr = False
    ser.rts = False                     # 不触发复位
    print(f"[bridge] 串口已开 {port_name} @115200 → 连接 127.0.0.1:{tcp_port}")

    sock = socket.create_connection(("127.0.0.1", tcp_port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    send_frame(sock, b"P", b"O")
    print("[bridge] 已接入中继，保持运行（Ctrl-C 退出）")

    stop = threading.Event()

    def ser_to_tcp():
        try:
            while not stop.is_set():
                d = ser.read(4096)
                if d:
                    send_frame(sock, b"D", d)
        except Exception as e:
            print(f"[bridge] 串口读线程结束: {e}")
        finally:
            stop.set()

    def tcp_to_ser():
        buf = b""
        try:
            while not stop.is_set():
                d = sock.recv(65536)
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
                        ser.write(payload)
        except Exception as e:
            print(f"[bridge] 网络线程结束: {e}")
        finally:
            stop.set()

    threading.Thread(target=ser_to_tcp, daemon=True).start()
    threading.Thread(target=tcp_to_ser, daemon=True).start()
    try:
        while not stop.is_set():
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    print("[bridge] 退出")
    try:
        sock.close(); ser.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()
