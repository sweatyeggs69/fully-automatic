"""Builds a tiny synthetic XDVDFS image with a fake default.xex for testing."""
import struct, sys

SECTOR = 2048

def entry(left, right, sector, size, attr, name):
    b = struct.pack("<HHIIBB", left, right, sector, size, attr, len(name)) + name
    return b + b"\xff" * ((-len(b)) % 4)

def build(path):
    xex = bytearray(b"XEX2")
    xex += struct.pack(">IIII", 1, 0x1000, 0, 0x200)      # flags, pe, reserved, security
    xex += struct.pack(">I", 2)                              # 2 optional headers
    xex += struct.pack(">II", 0x10100, 0x82000000)           # entry point
    xex += struct.pack(">II", 0x40006, 0x100)                # execution info offset
    xex += b"\0" * (0x100 - len(xex))
    xex += struct.pack(">IIII", 0xAABBCCDD, 0x00000001, 0, 0x4D5307E6)
    xex += b"\0" * (0x204 - len(xex)) + struct.pack(">I", 0x00400000)
    # Root dir: default.xex (sector 34) and Media/ (sector 35) containing hello.txt (sector 36)
    # build as a tree: first entry "default.xex" with right -> "Media"
    e0 = entry(0, 0, 34, len(xex), 0x20, b"default.xex")
    e1_off = len(e0) // 4
    e0 = entry(0, e1_off, 34, len(xex), 0x20, b"default.xex")
    root = e0 + entry(0, 0, 35, SECTOR, 0x10, b"Media")
    sub = entry(0, 0, 36, 5, 0x20, b"hello.txt")
    img = bytearray(37 * SECTOR + SECTOR)
    img[32 * SECTOR:32 * SECTOR + 28] = b"MICROSOFT*XBOX*MEDIA" + struct.pack("<II", 33, len(root))
    img[33 * SECTOR:33 * SECTOR + len(root)] = root
    img[34 * SECTOR:34 * SECTOR + len(xex)] = xex
    img[35 * SECTOR:35 * SECTOR + len(sub)] = sub
    img[36 * SECTOR:36 * SECTOR + 5] = b"hello"
    open(path, "wb").write(img)

if __name__ == "__main__":
    build(sys.argv[1])
