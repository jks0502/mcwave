"""Functional test of mcwave's cloud-album playback through the gateway (production D1, restores the baseline).

Drives epaper_control exactly like the web page does (revision+1), then watches the gateway log for
'[album] rendered revision=N id=XXXXXXXX'. usage: python album_scenario.py <gateway.log> <legacy-photos.json>
"""
import json
import re
import subprocess
import sys
import time

LOG, PHOTOS = sys.argv[1], sys.argv[2]
SITE = r"D:\codex\esp32\product2\personal-site"
photos = json.load(open(PHOTOS, encoding="utf-8"))
ids = [p["id"] for p in photos]
short = [i[:8] for i in ids]
EDISON_PHOTO = "80f2e53f"
BASE = {"mode": "album", "photo_id": ids[0], "paused": 0, "random_order": 0, "interval_seconds": 60}


def d1(sql):
    cmd = ["cmd", "/c", "npx", "wrangler", "d1", "execute", "epaper-control", "--remote", "--config", "wrangler.v6.production.jsonc", "--json", "--command", sql]
    out = subprocess.run(cmd, cwd=SITE, capture_output=True, text=True, encoding="utf-8").stdout
    return json.loads(out[out.index("["):])[0]["results"]


def set_state(**kv):
    now = int(time.time() * 1000)
    cols = ",".join("%s=%s" % (k, ("'%s'" % v) if isinstance(v, str) else v) for k, v in kv.items())
    rev = d1("UPDATE epaper_control SET %s,page=0,revision=revision+1,requested_at=%d WHERE id=1 RETURNING revision" % (cols, now))[0]["revision"]
    print(time.strftime("%H:%M:%S"), "set", kv.get("mode"), {k: (v[:8] if isinstance(v, str) and len(v) > 20 else v) for k, v in kv.items() if k != "mode"}, "-> revision", rev, flush=True)
    return rev


def renders_since(offset):
    text = open(LOG, encoding="utf-8", errors="replace").read()
    out = []
    for m in re.finditer(r"^(\d\d\d\d-\d\d-\d\d (\d\d:\d\d:\d\d)),\d+ \[device\] \[album\] rendered revision=(\d+) id=([0-9a-f]{8}) paused=(\d) interval=(\d+)", text[offset:], re.M):
        out.append((m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6))))
    return out


def log_size():
    return len(open(LOG, encoding="utf-8", errors="replace").read())


def wait_render(offset, predicate, timeout):
    end = time.time() + timeout
    while time.time() < end:
        for r in renders_since(offset):
            if predicate(r):
                return r
        time.sleep(3)
    return None


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name, detail, flush=True)


# A. paused on a chosen photo: shows it and does not advance.
off = log_size()
rev = set_state(mode="album", photo_id=ids[5], paused=1, random_order=0, interval_seconds=60)
r = wait_render(off, lambda x: x[1] == rev, 120)
check("A1 paused: chosen photo rendered", bool(r) and r[2] == short[5], str(r))
off = log_size()
time.sleep(150)
extra = renders_since(off)
check("A2 paused: no advance for 150 s", not extra, str(extra))

# B. sequence playback: unpause -> next photo in order.
off = log_size()
rev = set_state(mode="album", photo_id=ids[5], paused=0, random_order=0, interval_seconds=60)
r1 = wait_render(off, lambda x: x[3] == 0 and x[2] != short[5], 200)
check("B1 sequence: advances to the next photo", bool(r1) and r1[2] == short[6], str(r1))
r2 = wait_render(off, lambda x: r1 and x[2] not in (short[5], r1[2]), 200)
check("B2 sequence: advances again in order", bool(r2) and r2[2] == short[7], str(r2))
if r1 and r2:
    t = lambda s: sum(int(p) * m for p, m in zip(s.split(":"), (3600, 60, 1)))
    check("B3 sequence: spacing >= 55 s", t(r2[0]) - t(r1[0]) >= 55, "%ds" % (t(r2[0]) - t(r1[0])))

# C. random playback: only this device's photos, never an immediate repeat.
off = log_size()
set_state(mode="album", photo_id=ids[0], paused=0, random_order=1, interval_seconds=60)
seen = []
end = time.time() + 260
while time.time() < end and len(seen) < 3:
    for r in renders_since(off):
        if r[1] and (not seen or r[0] != seen[-1][0]):
            seen.append(r)
    time.sleep(5)
tail_ids = [s[2] for s in seen]
check("C1 random: >= 3 renders", len(seen) >= 3, str(tail_ids))
check("C2 random: only legacy photos", all(i in short for i in tail_ids) and EDISON_PHOTO not in tail_ids, str(tail_ids))
check("C3 random: no immediate repeat", all(a != b for a, b in zip(tail_ids, tail_ids[1:])), str(tail_ids))

# D. interval 300 s.
off = log_size()
set_state(mode="album", photo_id=ids[0], paused=0, random_order=0, interval_seconds=300)
a = wait_render(off, lambda x: x[4] == 300, 120)
b = wait_render(off, lambda x: a is not None and x[0] != a[0] and x[4] == 300, 420) if a else None
t = lambda s: sum(int(p) * m for p, m in zip(s.split(":"), (3600, 60, 1)))
check("D1 interval 300 s: next photo after >= 280 s", bool(a and b) and t(b[0]) - t(a[0]) >= 280, "%s -> %s" % (a, b))

# restore the baseline exactly.
set_state(**BASE)
print(json.dumps([{"name": n, "ok": ok, "detail": d} for n, ok, d in results], ensure_ascii=False))
sys.exit(0 if all(ok for _, ok, _ in results) else 1)
