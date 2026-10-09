#!/usr/bin/env python3
"""Spike C stress test: N parallel /jpg loops for D seconds, then /stats.

    python3 tools/spike_net_stress.py --host 192.168.4.1 --conns 6 --secs 600
"""
import argparse, threading, time, urllib.request

def worker(url, stop, stats, i):
    ok = err = 0
    while not stop.is_set():
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                body = r.read()
                if body[:2] == b"\xff\xd8" and body[-2:] == b"\xff\xd9":
                    ok += 1
                else:
                    err += 1
        except Exception:
            err += 1
    stats[i] = (ok, err)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--conns", type=int, default=6)
    ap.add_argument("--secs", type=int, default=600)
    a = ap.parse_args()
    base = "http://%s" % a.host
    before = urllib.request.urlopen(base + "/stats", timeout=5).read().decode()
    stop, stats = threading.Event(), {}
    ts = [threading.Thread(target=worker, args=(base + "/jpg", stop, stats, i))
          for i in range(a.conns)]
    t0 = time.time()
    for t in ts: t.start()
    time.sleep(a.secs); stop.set()
    for t in ts: t.join()
    after = urllib.request.urlopen(base + "/stats", timeout=5).read().decode()
    ok = sum(s[0] for s in stats.values()); err = sum(s[1] for s in stats.values())
    print("frames ok %d, errors %d, %.1f req/s" % (ok, err, ok / (time.time() - t0)))
    print("stats before:", before); print("stats after: ", after)
    print("PASS" if err == 0 else "FAIL")

if __name__ == "__main__":
    main()
