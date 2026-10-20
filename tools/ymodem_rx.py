import serial,time,glob
SOH,STX,EOT,ACK,NAK,CA,CREQ=0x01,0x02,0x04,0x06,0x15,0x18,0x43
P128,P1K=128,1024
def recv(s,path):
    f=open(path,'wb'); total=0; eot=0; first=True; expected=1
    while True:
        b=s.read(1)
        if not b: print("  timeout"); break
        c=b[0]
        if c==EOT:
            s.write(bytes([ACK])); s.flush(); eot+=1
            if eot>=2: break
            continue
        if c in (SOH,STX):
            n=P128 if c==SOH else P1K
            rest=s.read(n+4)
            if len(rest)<n+4: print("  partial"); break
            seq,nseq,pay=rest[0],rest[1],rest[2:2+n]
            if (seq^nseq)&0xff!=0xff:
                s.write(bytes([NAK])); s.flush(); continue
            if first:
                first=False; s.write(bytes([ACK])); s.flush(); s.write(bytes([CREQ])); s.flush()
                expected=1; continue
            if eot>=1:
                s.write(bytes([ACK])); s.flush(); continue
            if seq!=(expected&0xff):
                s.write(bytes([NAK])); s.flush(); continue
            f.write(pay); total+=len(pay); expected=(expected+1)&0xff
            s.write(bytes([ACK])); s.flush()
        elif c==CA:
            print("  aborted"); break
    f.close(); return total
def openp(baud=921600):
    return serial.Serial(sorted(glob.glob('/dev/ttyUSB*'))[0],baud,timeout=0.4)
def read_part(s,pid,out):
    s.reset_input_buffer(); time.sleep(0.3)
    s.write(("3 -id %d -r"%pid).encode()+b'\r'); s.flush()
    end=time.time()+4; buf=b''
    while time.time()<end:
        d=s.read(128)
        if d: buf+=d; end=time.time()+0.3
        if b"Select Receive File" in buf: break
    if b"Select Receive File" not in buf: return -1
    s.write(bytes([CREQ])); s.flush()
    return recv(s,out)
