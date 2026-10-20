#!/usr/bin/env python3
"""通过 MiCO bootloader 的 PARUPDATE 读回分区 (YMODEM), 用于备份。"""
import serial, time, sys, glob, os

SOH, STX, EOT, ACK, NAK, CA, CREQ = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
P128, P1K = 128, 1024

def openp(baud=921600):
    port = sorted(glob.glob('/dev/ttyUSB*'))[0]
    s = serial.Serial(port, baud, timeout=0.3)
    s.reset_input_buffer()
    return s

def cmd(s, c, wait=3):
    s.write(c.encode() + b'\r\n'); s.flush()
    end = time.time() + wait; data = b''
    while time.time() < end:
        d = s.read(256)
        if d:
            data += d; end = time.time() + 0.3
    return data

def ymodem_recv(s, out_path, verbose=True):
    f = open(out_path, 'wb'); total = 0; eot = 0
    while True:
        c = s.read(1)
        if not c:
            if verbose: print("  超时")
            break
        c = c[0]
        if c == EOT:
            s.write(bytes([ACK])); s.flush(); eot += 1
            if eot >= 2: break
            continue
        if c in (SOH, STX):
            size = P128 if c == SOH else P1K
            rest = s.read(size + 4)
            if len(rest) < size + 4:
                if verbose: print("  包不完整")
                break
            seq = rest[0]; nseq = rest[1]; payload = rest[2:2+size]
            if (seq ^ nseq) & 0xFF != 0xFF:
                s.write(bytes([NAK])); s.flush(); continue
            if seq == 0:
                # filename or final empty block0
                if payload[0] == 0:
                    s.write(bytes([ACK])); s.flush()
                else:
                    name = payload.split(b'\x00')[0].decode('latin1')
                    sz = payload.split(b'\x00', 1)[1].split(b'\x00')[0].split(b' ')[0].decode('latin1')
                    if verbose: print("  文件: %s  大小: %s" % (name, sz))
                    s.write(bytes([ACK])); s.flush()
                    s.write(bytes([CREQ])); s.flush()
            else:
                f.write(payload); total += len(payload)
                s.write(bytes([ACK])); s.flush()
                if verbose and total % (128*1024) < size:
                    print("    已收 %d 字节" % total)
        elif c == CA:
            if verbose: print("  设备中止")
            break
        else:
            pass
    f.close()
    return total

def read_partition(s, pid, out_path):
    s.write(("3 -id %d -r" % pid).encode() + b'\r\n'); s.flush()
    # wait for "Select Receive File"
    end = time.time() + 5; buf = b''
    while time.time() < end:
        d = s.read(128)
        if d:
            buf += d
            if b"Select Receive File" in buf:
                break
        if b"MXCHIP>" in buf and b"Select Receive File" not in buf:
            # command failed
            print("  分区 %d 读取失败: %r" % (pid, buf[-120:])); return 0
    s.write(bytes([CREQ])); s.flush()
    n = ymodem_recv(s, out_path)
    if n == 0:
        try: os.remove(out_path)
        except OSError: pass
    return n

if __name__ == "__main__":
    pid = int(sys.argv[1]); out = sys.argv[2]
    s = openp(921600)
    time.sleep(0.3)
    n = read_partition(s, pid, out)
    print("[+] 分区 %d -> %s : %d 字节" % (pid, out, n))
    s.close()
