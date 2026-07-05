#!/usr/bin/env python3
"""Send a command to the BlenderMCP addon socket (127.0.0.1:9876) directly,
bypassing the MCP client. Usage:
  py -3 tools/blender_cmd.py <type> [json_params]
  py -3 tools/blender_cmd.py execute_code '{"code":"import bpy; print(len(bpy.data.objects))"}'
Reads code from stdin if params is '-'."""
import socket, json, sys
def send(cmd_type, params=None, host='127.0.0.1', port=9876, timeout=120):
    s=socket.socket(socket.AF_INET,socket.SOCK_STREAM); s.settimeout(timeout)
    s.connect((host,port))
    s.sendall(json.dumps({'type':cmd_type,'params':params or {}}).encode())
    chunks=[]
    while True:
        try:
            d=s.recv(65536)
        except socket.timeout: break
        if not d: break
        chunks.append(d)
        try:
            json.loads(b''.join(chunks).decode()); break
        except Exception: continue
    s.close()
    return b''.join(chunks).decode()
if __name__=='__main__':
    t=sys.argv[1]; p={}
    if len(sys.argv)>2:
        raw=sys.stdin.read() if sys.argv[2]=='-' else sys.argv[2]
        p=json.loads(raw) if t!='execute_code' else {'code':raw} if sys.argv[2]=='-' else json.loads(raw)
    print(send(t,p))
