import socket,time,sys
def mon(cmds, drain=True):
    s=socket.socket(socket.AF_UNIX); s.connect('/tmp/m1-mon.sock'); time.sleep(.4)
    try: s.recv(8192)
    except: pass
    for c in cmds:
        s.send((c+'\n').encode()); time.sleep(.8)
        if drain:
            s.settimeout(.6)
            try:
                while True: s.recv(8192)
            except: pass
    s.close()
if __name__=='__main__': mon(sys.argv[1:])
