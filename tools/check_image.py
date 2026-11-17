# PlatformIO post-build check: refuse an image that could not run from the
# FNIRSI application slot, keep the bootloader's font area intact, and write
# the file for the bootloader's USB drive.
import hashlib
import os
import struct
Import("env")  # noqa: F821

APP_BASE = 0x08003C00
APP_MAX = 0x0800F800 - APP_BASE          # 48128 bytes
RAM_LO, RAM_HI = 0x20000000, 0x20005000
# The FNIRSI bootloader draws its own screen with the font table at
# 0x08008000 inside the application slot (the stock logo follows it). The
# linker script leaves 0x08008000..0x080099FF free; it gets the stock bytes.
HOLE = (0x08008000 - APP_BASE, 0x08009A00 - APP_BASE)
# Images smaller than this were not started by the bootloader (v0.1 test).
PAD_TO = 40 * 1024
# The stock FNIRSI application file is not part of this repository: put it at
# stock/DSO152_APP_V0.1.bin, or give its path in DSO152_STOCK.
PROJECT = env.subst("$PROJECT_DIR")  # noqa: F821
STOCK = os.environ.get("DSO152_STOCK", os.path.join(PROJECT, "stock", "DSO152_APP_V0.1.bin"))
STOCK_SHA = "4b79cb402d3e453f01f1b2a72a08349c770d2bf777c0899bc09b0fea26b2631e"
# Optional folder the finished file is also copied to: DSO152_DROP, or the
# first line of tools/drop_dir.txt (a local file, not in the repository).
DROP = os.environ.get("DSO152_DROP", "")
_drop_file = os.path.join(PROJECT, "tools", "drop_dir.txt")
if not DROP and os.path.isfile(_drop_file):
    DROP = open(_drop_file, encoding="utf-8").readline().strip()


def check(source, target, env):
    path = str(target[0])
    data = bytearray(open(path, "rb").read())
    sp, reset = struct.unpack_from("<II", data, 0)
    errors = []
    if len(data) > APP_MAX:
        errors.append("image %d bytes > %d" % (len(data), APP_MAX))
    if not (RAM_LO < sp <= RAM_HI):
        errors.append("initial SP 0x%08X outside RAM" % sp)
    if not (reset & 1) or not (APP_BASE <= (reset & ~1) < APP_BASE + HOLE[0]):
        errors.append("reset vector 0x%08X outside the code area" % reset)
    if len(data) < PAD_TO:
        data += b"\xFF" * (PAD_TO - len(data))
    if any(b not in (0x00, 0xFF) for b in data[HOLE[0]:HOLE[1]]):
        errors.append("something was linked into the bootloader font area")
    code_end = len(bytes(data[:HOLE[0]]).rstrip(b"\x00\xFF"))
    print("DSO152 image: code %d / %d bytes below the font area, total %d / %d"
          % (code_end, HOLE[0], len(data), APP_MAX))

    stock = b""
    if os.path.isfile(STOCK):
        s = open(STOCK, "rb").read()
        if hashlib.sha256(s).hexdigest() == STOCK_SHA:
            stock = s
    if stock:
        data[HOLE[0]:HOLE[1]] = stock[HOLE[0]:HOLE[1]]
        print("Bootloader font area 0x08008000-0x080099FF filled from the stock image")
    else:
        print("WARNING: stock image not found or changed; bootloader screen will show bars")

    if errors:
        os.remove(path)
        env.Exit("DSO152 image check FAILED: " + "; ".join(errors))

    out = os.path.join(env.subst("$PROJECT_DIR"), "out")
    os.makedirs(out, exist_ok=True)
    for d in (out, DROP):
        if not d or (d == DROP and not os.path.isdir(os.path.dirname(DROP))):
            continue
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "DSO152_APP_V0.1.bin"), "wb") as f:
            f.write(data)
    print("Wrote DSO152_APP_V0.1.bin (%d bytes) to out/%s" % (len(data), " and " + DROP if DROP else ""))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check)  # noqa: F821
