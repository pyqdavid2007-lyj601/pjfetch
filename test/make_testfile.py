#!/usr/bin/env python3
"""make_testfile.py — 生成与 range_server.py 完全相同内容的本地文件 / 哈希。

内容模型与 range_server.py 完全一致（两处代码同步修改）：
  32 位 LCG（A=1103515245，C=12345，mod 2^32，byte=(x>>16)&0xFF）预生成
  BLOCK=1MiB 表；逻辑流 = 周期重复 byte[pos] = table[pos % BLOCK]。

用法：
  python test/make_testfile.py --size 104857600 --dump out.bin   # 写文件，供 certutil -hashfile 比对
  python test/make_testfile.py --size 104857600 --hash           # 直接打印 sha256
  两者可同时给。
"""
import argparse
import hashlib
import sys

BLOCK = 1 << 20          # 必须与 range_server.py 一致
A, C, M = 1103515245, 12345, 1 << 32


def lcg_table(nbytes, seed):
    x = seed & 0xFFFFFFFF
    out = bytearray(nbytes)
    for i in range(nbytes):
        x = (A * x + C) % M
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def stream_chunks(size, seed, chunk=BLOCK):
    """按序产出逻辑流的 bytes 切片（table 周期重复）。"""
    t = lcg_table(min(BLOCK, size), seed)
    b = len(t)
    full = size // b
    tail = size - full * b
    big = t * full
    for off in range(0, len(big), chunk):
        yield big[off:off + chunk]
    if tail:
        yield t[:tail]


def main():
    ap = argparse.ArgumentParser(description="generate range_server-identical content")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=1 << 20)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--dump", metavar="FILE", help="write content to FILE")
    ap.add_argument("--hash", action="store_true", help="print sha256 hex of content")
    args = ap.parse_args()
    if not args.dump and not args.hash:
        ap.error("nothing to do: give --dump and/or --hash")

    if args.dump:
        with open(args.dump, "wb") as f:
            for piece in stream_chunks(args.size, args.seed):
                f.write(piece)
        print("wrote %s (%d bytes, seed=%d)" % (args.dump, args.size, args.seed))

    if args.hash:
        h = hashlib.sha256()
        for piece in stream_chunks(args.size, args.seed):
            h.update(piece)
        print(h.hexdigest())


if __name__ == "__main__":
    main()
