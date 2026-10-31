import socket
# test host port only: this helper must never talk to the real game (port 6400)
PORT = 6401
def send(cmd,timeout=15):
    try:
        s=socket.create_connection(('127.0.0.1',PORT),3); s.settimeout(timeout)
        s.sendall((cmd+'\n').encode()); buf=b''
        while True:
            d=s.recv(65536)
            if not d: break
            buf+=d
            l=buf.decode(errors='replace').strip().splitlines()
            if l and (l[-1]=='OK' or l[-1].startswith('ERR')): break
        s.close(); return buf.decode(errors='replace').strip()
    except Exception as e: return 'EXC '+str(e)
