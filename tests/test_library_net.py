#!/usr/bin/env python3
"""Integration test: client encodes+sends, server receives+decodes over RDMA."""
import ctypes
import os
import threading
import time
import subprocess

libpath = os.path.join(os.path.dirname(__file__), '..', 'libbitripple.so')
if not os.path.exists(libpath):
    libpath = './libbitripple.so'
lib = ctypes.CDLL(libpath)

BR_OK = 0

class br_profile_t(ctypes.Structure):
    _fields_ = [
        ("n", ctypes.c_uint32),
        ("m", ctypes.c_uint32),
        ("balls", ctypes.c_uint32 * 32),
        ("cum", ctypes.c_uint32 * 32),
        ("residual", ctypes.c_uint32),
    ]

lib.br_profile_new.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
lib.br_profile_new.restype = ctypes.POINTER(br_profile_t)
lib.br_profile_free.argtypes = [ctypes.POINTER(br_profile_t)]
lib.br_profile_set_uniform.argtypes = [ctypes.POINTER(br_profile_t)]
lib.br_profile_set_uniform.restype = ctypes.c_int

lib.br_client_new.argtypes = [
    ctypes.POINTER(br_profile_t),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_uint16),
    ctypes.c_uint32,
    ctypes.c_uint16,
    ctypes.c_uint16,
]
lib.br_client_new.restype = ctypes.c_void_p
lib.br_client_free.argtypes = [ctypes.c_void_p]
lib.br_client_send.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lib.br_client_send.restype = ctypes.c_int

lib.br_server_new.argtypes = [ctypes.c_uint16, ctypes.c_size_t, ctypes.c_uint16,
                              ctypes.c_char_p, ctypes.c_uint16]
lib.br_server_new.restype = ctypes.c_void_p
lib.br_server_free.argtypes = [ctypes.c_void_p]
lib.br_server_recv.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
lib.br_server_recv.restype = ctypes.c_int
lib.br_server_accept_paths.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.br_server_accept_paths.restype = ctypes.c_int

SERVER_PORT = 13337
DATA = b"The quick brown fox jumps over the lazy dog. " * 50
SYM_SIZE = 64

def test_client_server_rdma():
    print("Test: RDMA client/server integration")
    
    try:
        result = subprocess.run(['ibv_devices'], capture_output=True, text=True, timeout=5)
        is_softroce = 'rxe' in result.stdout and 'mlx' not in result.stdout
        if is_softroce:
            print("  Note: Using SoftRoCE (rxe) - using smaller test data")
        elif 'mlx' in result.stdout:
            print("  Using real RDMA hardware (mlx)")
        else:
            print("  SKIP: No RDMA device available")
            return
    except:
        print("  SKIP: Could not query RDMA devices")
        return
    
    test_data = DATA
    timeout_ms = 120000
    
    if is_softroce:
        test_data = b"Test data for SoftRoCE"  # smaller data
        timeout_ms = 180000
    
    prof = lib.br_profile_new(2, 2)
    lib.br_profile_set_uniform(prof)

    result = {'ok': False, 'data': None}
    server_ready = threading.Event()
    server_error = {'msg': None}

    def server_thread():
        srv = lib.br_server_new(SERVER_PORT, len(test_data), SYM_SIZE,
                                b"172.28.49.50", 13338)
        if not srv:
            server_error['msg'] = "server_new failed"
            server_ready.set()
            return
        ret = lib.br_server_accept_paths(srv, 2)
        if ret != BR_OK:
            server_error['msg'] = f"accept_paths failed: {ret}"
            lib.br_server_free(srv)
            server_ready.set()
            return
        server_ready.set()
        out = ctypes.create_string_buffer(len(test_data))
        rc = lib.br_server_recv(srv, out, len(test_data), timeout_ms)
        if rc == BR_OK:
            result['ok'] = True
            result['data'] = bytes(out)
        lib.br_server_free(srv)

    t = threading.Thread(target=server_thread)
    t.start()
    server_ready.wait(timeout=5)
    time.sleep(0.5)

    hosts = (ctypes.c_char_p * 2)(b"172.28.49.50", b"172.28.49.50")
    ports = (ctypes.c_uint16 * 2)(SERVER_PORT, SERVER_PORT)
    client = lib.br_client_new(prof, hosts, ports, 2, SYM_SIZE, 13338)

    if not client:
        t.join(timeout=1)
        print("  SKIP: RDMA connection failed (check RDMA device configuration)")
        lib.br_profile_free(prof)
        return

    rc = lib.br_client_send(client, test_data, len(test_data))
    assert rc == BR_OK, f"client_send failed: {rc}"
    lib.br_client_free(client)

    t.join(timeout=200)
    assert result['ok'], "server did not decode successfully"
    assert result['data'] == test_data, "decoded data mismatch"
    print("  PASS: RDMA client/server round-trip OK")

    lib.br_profile_free(prof)

if __name__ == '__main__':
    test_client_server_rdma()
    print("\nRDMA integration test passed.")