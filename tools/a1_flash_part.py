#!/usr/bin/env python3
"""把镜像写入指定分区 (MiCO bootloader PARUPDATE + YMODEM 发送)
用法: a1_flash_part.py <partition_id> <image.ota.bin>"""
import sys, glob, time, serial
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a1_send import sync_menu, send_file, read_for

def main():
    pid = int(sys.argv[1]); img = sys.argv[2]
    data = open(img, 'rb').read()
    s = serial.Serial(sorted(glob.glob('/dev/ttyUSB*'))[0], 921600, timeout=0.1)
    def ensure_menu():
        if sync_menu(s): return True
        s.write(b'a'); s.flush(); time.sleep(0.8); return sync_menu(s)
    if not ensure_menu():
        print("无法进入 bootloader 菜单"); return 1
    print("flashing %s (%d bytes) -> partition %d" % (img, len(data), pid))
    s.reset_input_buffer(); s.write(("3 -id %d\r" % pid).encode()); s.flush()
    buf = b''; end = time.time() + 4
    while time.time() < end:
        d = s.read(64)
        if d: buf += d; end = time.time() + 0.3
        if b'Waiting for the file' in buf: break
    if b'Waiting for the file' not in buf:
        print("设备未进入下载模式:", buf[-80:]); return 1
    ok = send_file(s, img.split('/')[-1][:64], data,
                   log=lambda *a: print(a[0]) if ('ack' in str(a[0]) or 'ready' in str(a[0]) or 'ok' in str(a[0])) else None)
    print("RESULT:", ok)
    print("tail:", read_for(s, 0.6)[-70:])
    s.close()
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
