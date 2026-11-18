"""Maps a bprof histogram to source lines.

    python3 tools/prof/sym.py BINARY HIST [TOP]

BINARY is the bprof binary that produced HIST (built non-PIE, with -g in the library for line
numbers). Prints the TOP (default 40) source lines by share of samples, each with its function
and, when inlined, the outermost caller, then the 25 functions with the largest inclusive
share (a sample counts for every function in its inline chain).
"""
import collections
import subprocess
import sys

binp, hist = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
rows = [l.split() for l in open(hist) if l.strip()]
ips = [r[0] for r in rows]
cnt = [int(r[1]) for r in rows]

# addr2line -i prints one or more (function, line) pairs per address; a 0x0 address after each
# one (which resolves to "??", "??:0") marks where its inline chain ends.
args = []
for i in ips:
    args += ["0x" + i, "0x0"]
out = subprocess.run(["addr2line", "-f", "-C", "-i", "-e", binp] + args,
                     capture_output=True, text=True).stdout.split("\n")
recs, cur, k = [], [], 0
while k + 1 < len(out):
    f, l = out[k], out[k + 1]
    k += 2
    if f == "??" and l.startswith("??:0"):
        recs.append(cur)
        cur = []
    else:
        cur.append((f, l.split("/")[-1].split(" ")[0]))

tot = sum(cnt)
by_line = collections.Counter()
by_func = collections.Counter()
for r, c in zip(recs, cnt):
    if not r:
        continue
    by_line[r[0][1] + "  " + r[0][0][:50] + ("  <" + r[-1][0][:40] if len(r) > 1 else "")] += c
    for f in set(x[0][:60] for x in r):
        by_func[f] += c
print("total", tot)
for key, v in by_line.most_common(top):
    print(f"{100 * v / tot:5.1f}% {key}")
print("--- inclusive by function")
for key, v in by_func.most_common(25):
    print(f"{100 * v / tot:5.1f}% {key}")
