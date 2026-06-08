import socket, json, sys, time
sock_path, out_ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); 
for _ in range(60):
    try: s.connect(sock_path); break
    except Exception: time.sleep(0.5)
f = s.makefile('rw')
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        line = f.readline()
        if not line: return None
        r = json.loads(line)
        if 'return' in r or 'error' in r: return r
f.readline()  # greeting
cmd({"execute":"qmp_capabilities"})
r = cmd({"execute":"screendump","arguments":{"filename":out_ppm}})
print("screendump:", r)
