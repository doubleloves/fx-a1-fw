#!/usr/bin/env python3
"""
斐讯A1 / EMW3080B 刷写与备份工具 (MiCO bootloader + YMODEM)
用法:
  ./a1_flash.py --port /dev/ttyUSB0 menu          # 显示 bootloader 菜单和内存映射
  ./a1_flash.py --port /dev/ttyUSB0 backup DIR     # 备份 bootloader/app/ate/rf/param 分区
  ./a1_flash.py --port /dev/ttyUSB0 flash FILE.ota.bin   # 烧录应用(.ota.bin)
  ./a1_flash.py --port /dev/ttyUSB0 boot           # 启动应用(6)
说明: 需要先把模块 BOOT 引脚拉低并上电才能进入 bootloader 菜单。
"""
import argparse, os, sys, time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

SOH, STX, EOT, ACK, NAK, CA, CRC16REQ = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
PACKET_SIZE, PACKET_1K = 128, 1024


def crc16(data):
    wcrc = 0
    for c in data:
        for _ in range(8):
            treat = c & 0x80
            c = (c << 1) & 0xFF
            bcrc = (wcrc >> 8) & 0x80
            wcrc = (wcrc << 1) & 0xFFFF
            if treat != bcrc:
                wcrc ^= 0x1021
    return wcrc


def log(*a):
    print(*a)
    sys.stdout.flush()


class Boot:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.2, write_timeout=2)
        self.buf = b""

    # ---- low level ----
    def read_byte(self, timeout=1.0):
        end = time.time() + timeout
        while time.time() < end:
            c = self.ser.read(1)
            if c:
                return c[0]
        return None

    def read_until(self, token, timeout=3.0):
        end = time.time() + timeout
        data = b""
        while time.time() < end:
            c = self.ser.read(1)
            if c:
                data += c
                if data.endswith(token):
                    return data
        return data

    def send_cmd(self, cmd, wait_prompt=True, timeout=5.0):
        self.ser.reset_input_buffer()
        self.ser.write(cmd.encode() + b"\r\n")
        self.ser.flush()
        if wait_prompt:
            return self.read_until(b"MXCHIP> ", timeout)
        return b""

    def enter_bootloader(self):
        """等待 bootloader 横幅/提示并显示。"""
        log("[*] 等待 bootloader 菜单 (确认 BOOT 引脚已拉低并重新上电)...")
        data = self.read_until(b"MXCHIP> ", timeout=6.0)
        if b"MXCHIP>" in data:
            log("[+] 已进入 bootloader")
            log(data.decode(errors="replace"))
            return True
        log("[-] 未检测到 bootloader 提示, 收到:")
        log(data.decode(errors="replace"))
        return False

    # ---- menu ----
    def memory_map(self):
        out = self.send_cmd("5")
        log(out.decode(errors="replace"))

    # ---- YMODEM receive (device -> host) ----
    def ymodem_recv(self, out_path):
        log("[*] 请求发送 (YMODEM receive): %s" % out_path)
        f = open(out_path, "wb")
        total = 0
        got_eot = 0
        expected = 0
        while True:
            c = self.read_byte(timeout=15.0)
            if c is None:
                log("[-] 接收超时"); break
            if c == EOT:
                self.ser.write(bytes([ACK])); self.ser.flush()
                got_eot += 1
                if got_eot == 1:
                    continue
                else:
                    break
            if c in (SOH, STX):
                size = PACKET_SIZE if c == SOH else PACKET_1K
                rest = self.ser.read(size + 4)
                if len(rest) < size + 4:
                    log("[-] 包不完整"); break
                seq = rest[0]; nseq = rest[1]; payload = rest[2:2 + size]
                if (seq ^ nseq) & 0xFF != 0xFF:
                    self.ser.write(bytes([NAK])); self.ser.flush(); continue
                if seq == 0 and expected == 0:
                    # filename packet
                    name, sz = payload.split(b"\x00", 1)
                    sz = sz.split(b"\x00", 1)[0].split(b" ", 1)[0]
                    log("[+] 文件名: %s  大小: %s" % (name.decode(errors='replace'), sz.decode(errors='replace')))
                    self.ser.write(bytes([ACK])); self.ser.flush()
                    self.ser.write(bytes([CRC16REQ])); self.ser.flush()
                    expected = 1
                else:
                    f.write(payload); total += len(payload)
                    self.ser.write(bytes([ACK])); self.ser.flush()
                    expected += 1
                    if total % (256 * 1024) < size:
                        log("    已接收 %d 字节" % total)
            elif c == CA:
                log("[-] 设备中止"); break
            else:
                try: log("[.] 忽略字节 0x%02x" % c)
                except Exception: pass
        f.close()
        log("[+] 完成, 共 %d 字节 -> %s" % (total, out_path))
        return total

    # ---- YMODEM send (host -> device) ----
    def ymodem_send(self, filename, data):
        log("[*] 等待设备请求 (C)...")
        c = self.read_byte(timeout=10.0)
        if c != CRC16REQ:
            log("[-] 未收到 'C' (收到 %r)" % c); return False
        # block 0
        hdr = filename.encode() + b"\x00" + str(len(data)).encode() + b"\x00"
        hdr = hdr + b"\x00" * (PACKET_SIZE - len(hdr))
        self._send_packet(SOH, 0, hdr)
        if self.read_byte(3.0) != ACK:
            log("[-] block0 未 ACK"); return False
        if self.read_byte(3.0) != CRC16REQ:
            log("[-] block0 后未收到 'C'"); return False
        # data blocks
        seq = 1
        off = 0
        while off < len(data):
            chunk = data[off:off + PACKET_1K]
            if len(chunk) == PACKET_1K:
                self._send_packet(STX, seq, chunk)
            else:
                pad = chunk + b"\x1a" * (PACKET_SIZE - len(chunk))
                self._send_packet(SOH, seq, pad)
            off += len(chunk)
            if self.read_byte(5.0) != ACK:
                log("[-] 数据块 %d 未 ACK" % seq); return False
            seq = (seq + 1) & 0xFF
            if off % (128 * 1024) < PACKET_1K:
                log("    已发送 %d/%d" % (off, len(data)))
        # EOT
        self.ser.write(bytes([EOT])); self.ser.flush()
        if self.read_byte(5.0) != ACK:
            log("[-] EOT 未 ACK"); return False
        # final empty block0
        self._send_packet(SOH, 0, b"\x00" * PACKET_SIZE)
        if self.read_byte(3.0) != ACK:
            log("[-] 结束包未 ACK"); return False
        log("[+] 发送完成")
        return True

    def _send_packet(self, kind, seq, payload):
        pkt = bytes([kind, seq, (~seq) & 0xFF]) + payload
        pkt += bytes([(crc16(payload) >> 8) & 0xFF, crc16(payload) & 0xFF])
        self.ser.write(pkt); self.ser.flush()

    def close(self):
        self.ser.close()


def connect(args):
    bauds = [args.baud] if args.baud else [921600, 115200, 57600, 9600]
    for b in bauds:
        try:
            d = Boot(args.port, b)
        except Exception as e:
            sys.exit("无法打开串口 %s: %s" % (args.port, e))
        if d.enter_bootloader():
            log("[+] 串口 %s @ %d" % (args.port, b))
            return d
        d.close()
    sys.exit("[-] 所有波特率都未能进入 bootloader: %s" % bauds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("menu")
    p_b = sub.add_parser("backup"); p_b.add_argument("outdir")
    p_f = sub.add_parser("flash"); p_f.add_argument("file")
    p_u = sub.add_parser("read"); p_u.add_argument("--id", type=int, required=True); p_u.add_argument("out")
    sub.add_parser("boot")
    args = ap.parse_args()

    d = connect(args)
    try:
        if args.cmd == "menu":
            d.memory_map()
        elif args.cmd == "backup":
            os.makedirs(args.outdir, exist_ok=True)
            parts = {1: "bootloader.bin", 2: "application.bin", 3: "ate.bin",
                     4: "rf_firmware.bin", 5: "parameter1.bin", 6: "parameter2.bin"}
            for pid, name in parts.items():
                if pid == 2:
                    resp = d.send_cmd("1 -r", timeout=4.0)
                else:
                    resp = d.send_cmd("3 -id %d -r" % pid, timeout=4.0)
                log("[*] 分区 %d: %s" % (pid, resp.decode(errors="replace").strip().splitlines()[-1:]
                                          if resp else b""))
                d.ymodem_recv(os.path.join(args.outdir, name))
        elif args.cmd == "read":
            d.send_cmd("3 -id %d -r" % args.id, timeout=4.0)
            d.ymodem_recv(args.out)
        elif args.cmd == "flash":
            data = open(args.file, "rb").read()
            log("[*] 烧录 %s (%d 字节)" % (args.file, len(data)))
            d.send_cmd("1", wait_prompt=False)
            if d.ymodem_send(os.path.basename(args.file), data):
                log("[+] 烧录完成, 输入 7 重启 (或断电重启)")
                d.send_cmd("7", wait_prompt=False, timeout=3.0)
        elif args.cmd == "boot":
            d.send_cmd("6", wait_prompt=False)
    finally:
        d.close()


if __name__ == "__main__":
    main()
