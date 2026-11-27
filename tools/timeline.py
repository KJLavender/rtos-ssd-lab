#!/usr/bin/env python3
"""Draw a text timeline from the TR/TM lines printed by bsp/trace.c.

Usage: python3 tools/timeline.py <qemu output> [--ms-per-char N] [--tasks H,M,L]

One row per task, one character per time slot:
  #  running at its own priority
  ^  running at a raised (inherited) priority
  .  waiting for the lock (between "wants lock" and "got lock" marks)
"""
import argparse
import collections
import sys


def parse(path):
    switches = collections.defaultdict(list)   # tag -> [(t_us, task, prio)]
    marks = collections.defaultdict(list)      # tag -> [(t_us, task, label)]
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.strip().split(",", 4)
            if parts[0] == "TR" and len(parts) == 5:
                switches[parts[1]].append((int(parts[2]), parts[3], int(parts[4])))
            elif parts[0] == "TM" and len(parts) == 5:
                marks[parts[1]].append((int(parts[2]), parts[3], parts[4]))
    return switches, marks


def draw(tag, sw, mk, tasks, step_us):
    end_us = max(t for t, _, _ in sw) + step_us
    slots = end_us // step_us
    base = {}
    for _, task, prio in sw:
        base[task] = min(prio, base.get(task, prio))

    def running_at(t):
        cur = None
        for ts, task, prio in sw:
            if ts > t:
                break
            cur = (task, prio)
        return cur

    waiting = []   # (start, end) spans where H waits for the lock
    start = None
    for t, task, label in mk:
        if label.endswith("wants lock"):
            start = t
        elif label.endswith("got lock") and start is not None:
            waiting.append((start, t))
            start = None

    rows = {task: [" "] * slots for task in tasks}
    for i in range(slots):
        t = i * step_us + step_us // 2
        cur = running_at(t)
        if cur and cur[0] in rows:
            task, prio = cur
            rows[task][i] = "^" if prio > base[task] else "#"
        if "H" in rows and rows["H"][i] == " " and any(a <= t < b for a, b in waiting):
            rows["H"][i] = "."

    ruler = "".join(
        "|" if (i * step_us) % (10 * step_us) == 0 else "-" for i in range(slots))
    labels = "".join(
        f"{i * step_us // 1000:<10}" for i in range(0, slots, 10))

    print(f"[{tag}]  1 char = {step_us // 1000} ms")
    print(f"     {labels.rstrip()}")
    print(f"     {ruler}")
    for task in tasks:
        print(f"  {task:<2} {''.join(rows[task]).rstrip()}")
    for a, b in waiting:
        print(f"  H waited for the lock {(b - a) / 1000:.1f} ms")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--ms-per-char", type=int, default=1)
    ap.add_argument("--tasks", default="H,M,L")
    args = ap.parse_args()

    switches, marks = parse(args.log)
    if not switches:
        sys.exit("no TR lines found")
    print("# running   ^ running with inherited priority   . waiting for lock\n")
    for tag in switches:
        draw(tag, switches[tag], marks[tag], args.tasks.split(","),
             args.ms_per_char * 1000)


if __name__ == "__main__":
    main()
