#!/usr/bin/env python3
"""Mutation-based fuzzing driver for the knfmt fuzz harnesses.

Feeds mutated inputs to a standalone harness (built in the default AFL mode,
which reads one input from stdin) and records crashing or timing-out
reproducers. Usage:

    python3 tests/mutate.py <harness> [iterations] [outdir]

The harness must enforce the property under test itself: fuzz-parse traps on
non-idempotent formatting.
"""
import os
import random
import subprocess
import sys
import time

HARNESS = sys.argv[1] if len(sys.argv) > 1 else "./fuzz-parse"
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
OUT = sys.argv[3] if len(sys.argv) > 3 else "crashes"
SEED_DIRS = ["tests"]
TIMEOUT = 15

# Tokens that exercise recovery, comments, CPP and malformed constructs.
DICT = [
    b"\n", b"\t", b" ", b"/*", b"*/", b"//", b"//\n", b"\\\n",
    b"#if", b"#ifdef", b"#ifndef", b"#else", b"#elif", b"#endif", b"#define",
    b"#include", b"#undef", b"#error", b"(", b")", b"{", b"}", b"[", b"]",
    b";", b",", b"|", b"<", b">", b"||", b"&&", b"?", b":", b"\"", b"'",
    b"0", b"8", b"3600", b"0xffffffff", b"18446744073709551615", b"->",
    b"...", b"sizeof", b"_Generic", b"_Static_assert", b"static_assert",
    b"__attribute__", b"asm", b"static", b"struct", b"union", b"enum",
    b"NULL", b"nullptr", b"\x00", b"\xff", b"\r\n", b"\r", b"A" * 64,
]


def load_seeds():
    seeds = []
    for d in SEED_DIRS:
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            p = os.path.join(d, name)
            if os.path.isfile(p) and (name.endswith(".c") or name.endswith(".h")):
                try:
                    with open(p, "rb") as fh:
                        seeds.append(fh.read())
                except OSError:
                    pass
    return seeds


def mutate(data, seeds):
    data = bytearray(data)
    for _ in range(random.randint(1, 8)):
        op = random.randint(0, 6)
        if not data:
            data = bytearray(b"int x;\n")
        if op == 0:  # bit flip
            i = random.randrange(len(data))
            data[i] ^= 1 << random.randrange(8)
        elif op == 1:  # set byte
            data[random.randrange(len(data))] = random.randrange(256)
        elif op == 2:  # insert byte
            data.insert(random.randrange(len(data) + 1), random.randrange(256))
        elif op == 3:  # delete byte
            del data[random.randrange(len(data))]
        elif op == 4:  # insert dictionary token
            tok = random.choice(DICT)
            i = random.randrange(len(data) + 1)
            data[i:i] = tok
        elif op == 5:  # duplicate a chunk
            if len(data) > 1:
                a = random.randrange(len(data))
                b = min(len(data), a + random.randint(1, 32))
                i = random.randrange(len(data) + 1)
                data[i:i] = data[a:b]
        else:  # splice with another seed
            other = random.choice(seeds)
            if other:
                cut = random.randrange(len(data) + 1)
                data = bytearray(data[:cut]) + bytearray(other)
    return bytes(data)


def main():
    os.makedirs(OUT, exist_ok=True)
    seeds = load_seeds()
    if not seeds:
        print("no seeds", file=sys.stderr)
        return 2
    random.seed(20261010)
    execs = crashes = timeouts = 0
    started = time.time()
    for n in range(ITERS):
        data = mutate(random.choice(seeds), seeds)
        execs += 1
        try:
            r = subprocess.run([HARNESS], input=data,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=TIMEOUT)
            rc = r.returncode
        except subprocess.TimeoutExpired:
            timeouts += 1
            path = os.path.join(OUT, f"timeout-{n}.bin")
            with open(path, "wb") as fh:
                fh.write(data)
            print(f"TIMEOUT {path}", flush=True)
            continue
        if rc < 0 or rc >= 128:
            crashes += 1
            path = os.path.join(OUT, f"crash-{n}-sig{rc if rc >= 0 else -rc}.bin")
            with open(path, "wb") as fh:
                fh.write(data)
            print(f"CRASH rc={rc} {path}", flush=True)
    dur = time.time() - started
    print(f"target={HARNESS} seeds={len(seeds)} execs={execs} "
          f"crashes={crashes} timeouts={timeouts} duration={dur:.1f}s")
    return 1 if crashes or timeouts else 0


if __name__ == "__main__":
    sys.exit(main())
