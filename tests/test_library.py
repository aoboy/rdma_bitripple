#!/usr/bin/env python3
"""Unit tests for libbitripple (no network required)."""
import ctypes
import os

libpath = os.path.join(os.path.dirname(__file__), '..', 'libbitripple.so')
if not os.path.exists(libpath):
    libpath = './libbitripple.so'
lib = ctypes.CDLL(libpath)

BR_MAX_PATHS = 32
BR_OK = 0
BR_ERR_INVAL = -2


class br_profile_t(ctypes.Structure):
    _fields_ = [
        ("n", ctypes.c_uint32),
        ("m", ctypes.c_uint32),
        ("balls", ctypes.c_uint32 * BR_MAX_PATHS),
        ("cum", ctypes.c_uint32 * BR_MAX_PATHS),
        ("residual", ctypes.c_uint32),
    ]


class br_spray_t(ctypes.Structure):
    _fields_ = [
        ("j", ctypes.c_uint32),
        ("l", ctypes.c_uint32),
        ("sa", ctypes.c_uint32),
        ("sb", ctypes.c_uint32),
        ("method", ctypes.c_int),
    ]


# --- Bindings ---
lib.br_profile_new.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
lib.br_profile_new.restype = ctypes.POINTER(br_profile_t)

lib.br_profile_free.argtypes = [ctypes.POINTER(br_profile_t)]
lib.br_profile_free.restype = None

lib.br_profile_set_uniform.argtypes = [ctypes.POINTER(br_profile_t)]
lib.br_profile_set_uniform.restype = ctypes.c_int

lib.br_spray_new.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int]
lib.br_spray_new.restype = ctypes.POINTER(br_spray_t)

lib.br_spray_free.argtypes = [ctypes.POINTER(br_spray_t)]
lib.br_spray_free.restype = None

lib.br_spray_select.argtypes = [ctypes.POINTER(br_profile_t), ctypes.POINTER(br_spray_t), ctypes.c_uint32]
lib.br_spray_select.restype = ctypes.c_uint32

lib.br_encoder_new.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint16]
lib.br_encoder_new.restype = ctypes.c_void_p

lib.br_encoder_free.argtypes = [ctypes.c_void_p]
lib.br_encoder_free.restype = None

lib.br_encoder_num_blocks.argtypes = [ctypes.c_void_p]
lib.br_encoder_num_blocks.restype = ctypes.c_uint32

lib.br_encoder_block_symbols.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
lib.br_encoder_block_symbols.restype = ctypes.c_uint32

lib.br_encoder_symbol.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint32,
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)
]
lib.br_encoder_symbol.restype = ctypes.c_int

lib.br_encoder_oti.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32)]
lib.br_encoder_oti.restype = None

lib.br_decoder_new.argtypes = [ctypes.c_uint64, ctypes.c_uint32, ctypes.c_size_t]
lib.br_decoder_new.restype = ctypes.c_void_p

lib.br_decoder_free.argtypes = [ctypes.c_void_p]
lib.br_decoder_free.restype = None

lib.br_decoder_feed.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint32,
    ctypes.c_void_p, ctypes.c_size_t
]
lib.br_decoder_feed.restype = ctypes.c_int

lib.br_decoder_ready.argtypes = [ctypes.c_void_p]
lib.br_decoder_ready.restype = ctypes.c_int

lib.br_decoder_reconstruct.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lib.br_decoder_reconstruct.restype = ctypes.c_int


def test_profile():
    print("Test: profile creation and uniformity")
    p = lib.br_profile_new(4, 1024)
    assert p, "profile_new failed"
    rc = lib.br_profile_set_uniform(p)
    assert rc == BR_OK, f"set_uniform failed: {rc}"
    total = sum(p.contents.balls[i] for i in range(4))
    assert total == 1024, f"sum={total}, expected 1024"
    print("  PASS: uniform profile sums to m")
    lib.br_profile_free(p)


def test_spray():
    print("Test: deterministic spray over full cycle")
    p = lib.br_profile_new(2, 256)
    lib.br_profile_set_uniform(p)
    s = lib.br_spray_new(8, 33, 7, 1)
    assert s, "spray_new failed"
    counts = [0, 0]
    for j in range(256):
        path = lib.br_spray_select(p, s, j)
        counts[path] += 1
    assert counts[0] == 128 and counts[1] == 128, f"counts={counts}"
    print("  PASS: exact uniform distribution over 256 packets")
    lib.br_spray_free(s)
    lib.br_profile_free(p)


def test_codec_roundtrip():
    print("Test: encoder/decoder round-trip via nanorq")
    data = b"Hello, BitRipple! This is a test message for fountain coding." * 10
    sym_size = 32

    enc = lib.br_encoder_new(data, len(data), sym_size)
    assert enc, "encoder_new failed"

    num_blocks = lib.br_encoder_num_blocks(enc)
    print(f"  Blocks: {num_blocks}")

    oti_common = ctypes.c_uint64()
    oti_scheme = ctypes.c_uint32()
    lib.br_encoder_oti(enc, ctypes.byref(oti_common), ctypes.byref(oti_scheme))

    # Collect symbols for ALL blocks
    symbols = []  # list of (sbn, esi, bytes)
    buf = ctypes.create_string_buffer(sym_size)
    for sbn in range(num_blocks):
        num_src = lib.br_encoder_block_symbols(enc, sbn)
        print(f"  Block {sbn}: {num_src} source symbols")
        total = num_src + max(num_src // 2, 4)
        for esi in range(total):
            out_len = ctypes.c_size_t()
            rc = lib.br_encoder_symbol(enc, sbn, esi, buf, ctypes.byref(out_len))
            if rc == BR_OK:
                symbols.append((sbn, esi, bytes(buf[:out_len.value])))

    lib.br_encoder_free(enc)

    dec = lib.br_decoder_new(oti_common.value, oti_scheme.value, len(data))
    assert dec, "decoder_new failed"

    for sbn, esi, sym in symbols:
        rc = lib.br_decoder_feed(dec, sbn, esi, sym, len(sym))
        assert rc == BR_OK, f"feed failed for block {sbn} esi {esi}: {rc}"

    assert lib.br_decoder_ready(dec), "decoder not ready"
    out = ctypes.create_string_buffer(len(data))
    rc = lib.br_decoder_reconstruct(dec, out, len(data))
    assert rc == BR_OK, f"reconstruct failed: {rc}"
    assert bytes(out) == data, "data mismatch"
    print("  PASS: round-trip OK")
    lib.br_decoder_free(dec)


if __name__ == '__main__':
    test_profile()
    test_spray()
    test_codec_roundtrip()
    print("\nAll unit tests passed.")