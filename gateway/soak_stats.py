"""Summarise a gateway console log (python gateway.py --console): syncs, transfers, resends, boots."""
import json
import re
import sys

log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
out = {"lines": len(log)}
ts = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")
times = [m.group(1) for m in (ts.match(l) for l in log) if m]
out["first"], out["last"] = (times[0], times[-1]) if times else ("", "")
out["attached"] = sum("attached at" in l for l in log)
out["sync_ok"] = sum("[remote] sync OK" in l for l in log)
out["sync_failed"] = sum("[remote] sync failed" in l for l in log)
out["boots"] = sum("[boot] reset reason" in l for l in log)
out["gateway_lost"] = sum("gateway lost" in l for l in log)
out["usb_connect_ok"] = sum("[usb] connect" in l and "ok=1" in l for l in log)
out["usb_connect_failed"] = sum("[usb] connect" in l and "ok=0" in l for l in log)
out["weather_ok"] = sum("[weather] NMC updated" in l for l in log)
out["weather_failed"] = sum("[weather] result query_failed=1" in l for l in log)
out["rain_http200"] = sum("[rain] HTTP=200 bytes=" in l and "bytes=-" not in l for l in log)
out["rain_failed"] = sum("[rain] body failed" in l or "[rain] HTTP=-" in l for l in log)
out["album_download_ok"] = sum("[album] download HTTP=200" in l and "hash=OK" in l for l in log)
out["album_download_failed"] = sum("[album] download" in l and "hash=OK" not in l for l in log)
out["screen_mirror_204"] = sum("[screen-mirror] HTTP=204" in l for l in log)
closed = [re.search(r"closed (\S+) \(to device (\d+) bytes, (\d+) resends\)", l) for l in log]
closed = [m for m in closed if m]
out["connections"] = len(closed)
out["bytes_to_device"] = sum(int(m.group(2)) for m in closed)
out["resends"] = sum(int(m.group(3)) for m in closed)
out["never_confirmed_close"] = sum("never confirmed" in l for l in log)
stats = [re.search(r"uartErrors=(\d+) badFrames=(\d+)", l) for l in log]
stats = [m for m in stats if m]
out["uart_errors"], out["bad_frames"] = (int(stats[-1].group(1)), int(stats[-1].group(2))) if stats else (0, 0)
heaps = [int(m.group(1)) for m in (re.search(r"heap=(\d+)", l) for l in log) if m]
out["min_heap"] = min(heaps) if heaps else None
print(json.dumps(out, ensure_ascii=False, indent=1))
