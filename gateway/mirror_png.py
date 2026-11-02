"""Read mcwave's latest screen mirror from production D1 (read-only SELECT) and save it as a PNG.

usage: python mirror_png.py out.png [device_id]
The frame is 400x300, row-major, MSB-first, 1 = white (see SCREEN_MIRROR.md).
"""
import json
import os
import subprocess
import sys
import time

from PIL import Image

MCWAVE = "00000000-0000-0000-0000-00000000mcwave"
SITE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "personal-site")


def fetch(device_id=MCWAVE):
    sql = "SELECT hex(frame) AS h, hash, mode, updated_at FROM md_screens WHERE device_id='%s'" % device_id
    cmd = ["cmd", "/c", "npx", "wrangler", "d1", "execute", "epaper-control", "--remote", "--config",
           "wrangler.v6.production.jsonc", "--json", "--command", sql]
    out = subprocess.run(cmd, cwd=SITE, capture_output=True, text=True, encoding="utf-8").stdout
    return json.loads(out[out.index("["):])[0]["results"][0]


def to_image(hex_frame):
    data = bytes.fromhex(hex_frame)
    img = Image.new("1", (400, 300), 1)
    px = img.load()
    for y in range(300):
        for x in range(400):
            px[x, y] = (data[y * 50 + x // 8] >> (7 - x % 8)) & 1
    return img


if __name__ == "__main__":
    row = fetch(sys.argv[2] if len(sys.argv) > 2 else MCWAVE)
    to_image(row["h"]).convert("L").resize((800, 600), Image.NEAREST).save(sys.argv[1])
    print(json.dumps({"hash": row["hash"], "mode": row["mode"], "updated": time.strftime("%H:%M:%S", time.localtime(row["updated_at"] / 1000))}))
