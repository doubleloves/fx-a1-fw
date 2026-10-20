import serial,time,glob,hashlib,sys
SOH,STX,EOT,ACK,NAK,CA,CREQ=0x01,0x02,0x04,0x06,0x15,0x18,0x43
def pkt(kind,seq,pay):
    p=bytes([kind,seq,(~seq)&0xff])+pay
    return p+b'\x00\x00'   # device ignores CRC
def send(s,name,data):
    b=s.read(1)
    if b!=bytes([CREQ]): print("  no C, got",b); return False
    hdr=name.encode()+b'\x00'+str(len(data)).encode()+b'\x00'
    hdr=hdr+b'\x00'*(128-len(hdr))
    s.write(pkt(SOH,0,hdr)); s.flush()
    if s.read(1)!=bytes([ACK]): print("  block0 no ACK"); return False
    if s.read(1)!=bytes([CREQ]): print("  no C after block0"); return False
    seq=1; off=0
    while off<len(data):
        chunk=data[off:off+1024]
        if len(chunk)==1024: s.write(pkt(STX,seq,chunk))
        else: s.write(pkt(SOH,seq,chunk+b'\x1a'*(128-len(chunk))))
        s.flush()
        if s.read(1)!=bytes([ACK]): print("  block %d no ACK"%seq); return False
        off+=len(chunk); seq=(seq+1)&0xff
    s.write(bytes([EOT])); s.flush()
    if s.read(1)!=bytes([ACK]): print("  EOT no ACK"); return False
    s.write(pkt(SOH,0,b'\x00'*128)); s.flush()
    if s.read(1)!=bytes([ACK]): print("  final no ACK"); return False
    return True
if __name__=="__main__":
    s=serial.Serial(sorted(glob.glob('/dev/ttyUSB*'))[0],921600,timeout=1.0)
    s.reset_input_buffer(); time.sleep(0.3)
    test=bytes(range(256))*16  # 4096 bytes pattern
    print("writing test 4096 bytes to partition 5 (OTA Storage)...")
    s.write(b'3 -id 5\r'); s.flush()
    # wait for "Waiting for the file"
    end=time.time()+8; buf=b''
    while time.time()<end:
        d=s.read(128)
        if d: buf+=d; end=time.time()+0.5
        if b"Waiting for the file" in buf: break
    print("  dev:",buf[-60:])
    ok=send(s,"test.bin",test)
    print("  send ok:",ok)
    time.sleep(0.5)
    print("  tail:",s.read(200)[-80:])
    open('backup/write_test.bin','wb').write(test)
    s.close()
