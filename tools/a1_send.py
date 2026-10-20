import serial,time,glob
SOH,STX,EOT,ACK,NAK,CA,CREQ=0x01,0x02,0x04,0x06,0x15,0x18,0x43
def read_for(s,dur):
    end=time.time()+dur; b=b''
    while time.time()<end:
        d=s.read(256)
        if d: b+=d
    return b
def wait_target(s,target,to):
    end=time.time()+to
    while time.time()<end:
        b=s.read(1)
        if b==target: return True
    return False
def sync_menu(s):
    for _ in range(5):
        s.reset_input_buffer(); s.write(b'\r'); s.flush()
        if b'MXCHIP>' in read_for(s,0.8): return True
    return False
def send_file(s, name, data, log=print):
    hdr=name.encode()+b'\x00'+str(len(data)).encode()+b'\x00'; hdr=hdr+b'\x00'*(128-len(hdr))
    blk0=bytes([SOH,0,0xff])+hdr+b'\x00\x00'
    # retransmit block0 until ACK
    got=False
    for i in range(12):
        s.write(blk0); s.flush()
        if wait_target(s,bytes([ACK]),2.0): got=True; break
    if not got: log("  block0 never ACKed"); return False
    log("  block0 ACK")
    if not wait_target(s,bytes([CREQ]),25.0): log("  no C after block0"); return False
    log("  ready for data")
    seq=1; off=0
    while off<len(data):
        ch=data[off:off+1024]
        p=bytes([STX if len(ch)==1024 else SOH,seq,(~seq)&0xff])+(ch if len(ch)==1024 else ch+b'\x1a'*(128-len(ch)))+b'\x00\x00'
        okb=False
        for _ in range(12):
            s.write(p); s.flush()
            if wait_target(s,bytes([ACK]),3.0): okb=True; break
        if not okb: log("  data block %d failed"%seq); return False
        off+=len(ch); seq=(seq+1)&0xff
        if off%(64*1024)<1024: log("    %d/%d"%(off,len(data)))
    # EOT
    eok=False
    for _ in range(12):
        s.write(bytes([EOT])); s.flush()
        if wait_target(s,bytes([ACK]),3.0): eok=True; break
    if not eok: log("  EOT failed"); return False
    fin=bytes([SOH,0,0xff])+b'\x00'*128+b'\x00\x00'
    fok=False
    for _ in range(12):
        s.write(fin); s.flush()
        if wait_target(s,bytes([ACK]),3.0): fok=True; break
    log("  EOT ok, final ok=%s"%fok)
    return True
