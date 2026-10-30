import re, statistics as s, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
rows = []
splits = []
for i, l in enumerate(lines):
    if not l.startswith("[perf]"):
        continue
    m = re.search(r"draws (\d+)/", l)
    if not m or int(m.group(1)) <= 300:
        continue
    m2 = re.search(r"fps ([\d.]+) game ([\d.]+) speed (\d+)%.*frame avg ([\d.]+) max ([\d.]+).*"
                   r"guest ([\d.]+) render ([\d.]+) wait ([\d.]+)", l)
    if not m2:
        continue
    rows.append(tuple(float(x) for x in m2.groups()))
    for j in range(i + 1, min(i + 3, len(lines))):
        if lines[j].startswith("[render-split]"):
            m3 = re.search(r"lists ([\d.]+).*replay ([\d.]+) present ([\d.]+)", lines[j])
            if m3:
                splits.append(tuple(float(x) for x in m3.groups()))
if rows:
    med = lambda k: round(s.median(r[k] for r in rows), 2)
    print(len(rows), "race s: fps", med(0), "game", med(1), "speed", med(2), "frame", med(3),
          "guest", med(5), "render", med(6), "wait", med(7))
    work = sorted(r[5] + r[6] for r in rows)
    print("guest+render median", round(s.median(work), 2), "p90", round(work[int(len(work) * 0.9)], 2))
    print("seconds under 95% speed:", sum(1 for r in rows if r[2] < 95))
if splits:
    print("split median lists/replay/present:", [round(s.median(x[k] for x in splits), 2) for k in range(3)])
last = None
for l in lines:
    if "GPU time of the two eyes" in l:
        last = l

if last: print(last[:120])
