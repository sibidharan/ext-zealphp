#!/usr/bin/env python3
"""ext#59 burst classifier.

usage: burst.py PORT SCRIPT CONCURRENCY ROUNDS
OK    = request read back its own value
LEAK  = read a DIFFERENT request's value (any round — constants persist per worker)
UNDEF = constant lost
HANG  = timeout / no response
ERR   = non-200 or unparseable body
"""
import concurrent.futures
import json
import sys
import urllib.request
import uuid

port, script, conc, rounds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])


def fire(who):
    url = f"http://127.0.0.1:{port}/{script}?who={who}"
    try:
        with urllib.request.urlopen(url, timeout=20) as r:
            return who, r.status, r.read().decode()
    except Exception as e:
        return who, 0, str(e)


ok = leak = undef = hang = err = 0
leak_samples = []
err_samples = []
status_counts = {}
all_whos = set()
for rnd in range(rounds):
    whos = [f"r{rnd}w{i}-{uuid.uuid4().hex[:6]}" for i in range(conc)]
    all_whos.update(whos)
    with concurrent.futures.ThreadPoolExecutor(max_workers=conc) as ex:
        results = list(ex.map(fire, whos))
    for who, status, body in results:
        status_counts[status] = status_counts.get(status, 0) + 1
        if status == 0:
            hang += 1
            if len(err_samples) < 3:
                err_samples.append((who, status, body[:200]))
            continue
        if status != 200:
            err += 1
            if len(err_samples) < 3:
                err_samples.append((who, status, body[:200]))
            continue
        try:
            j = json.loads(body)
        except Exception:
            err += 1
            if len(err_samples) < 3:
                err_samples.append((who, status, body[:200]))
            continue
        c = j.get("const")
        if c == who:
            ok += 1
        elif c == "UNDEF" or c is None:
            undef += 1
        else:
            leak += 1
            if len(leak_samples) < 5:
                leak_samples.append((who, c, "peer" if c in all_whos else "foreign"))

total = ok + leak + undef + hang + err
print(f"OK={ok} LEAK={leak} UNDEF={undef} HANG={hang} ERR={err} total={total}")
print(f"leak_rate={leak / max(total, 1) * 100:.1f}%")
print(f"status_counts={status_counts}")
for a, b, kind in leak_samples:
    print(f"  leak: {a} read {b!r} ({kind})")
for who, status, body in err_samples:
    print(f"  err[{status}] {who}: {body!r}")
