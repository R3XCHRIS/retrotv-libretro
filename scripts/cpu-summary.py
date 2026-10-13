#!/usr/bin/env python3
"""Summarise per-thread CPU samples written by pi-run.sh (cpu.txt).

Prints average and peak CPU (% of one core) for the whole process, the main
(libretro) thread, the player thread (retrotv-dec) and FFmpeg's decoder
threads (named av:*), plus the peak thread count.
"""
import sys

HZ = 100  # getconf CLK_TCK on the Pi


def main(path):
    samples = []  # (uptime, {tid: (ticks, name)})
    for line in open(path):
        f = line.split(" ", 3)
        if f[0] == "S":
            samples.append((float(f[1]), {}))
        elif f[0] == "T" and samples:
            samples[-1][1][int(f[1])] = (int(f[2]), f[3].strip())
    if len(samples) < 2:
        print("cpu: not enough samples")
        return
    main_tid = min(samples[0][1])
    groups = {"process": [], "main": [], "retrotv-dec": [], "ffmpeg av:*": []}
    for (t0, a), (t1, b) in zip(samples, samples[1:]):
        dt = t1 - t0
        if dt <= 0:
            continue
        g = dict.fromkeys(groups, 0.0)
        for tid, (ticks, name) in b.items():
            d = (ticks - a.get(tid, (0, ""))[0]) / HZ / dt * 100  # new thread: counts from 0
            g["process"] += d
            if tid == main_tid:
                g["main"] += d
            elif name == "retrotv-dec":
                g["retrotv-dec"] += d
            elif name.startswith("av:"):
                g["ffmpeg av:*"] += d
        for k in groups:
            groups[k].append(g[k])
    peak_threads = max(len(s[1]) for s in samples)
    print(f"cpu over {samples[-1][0] - samples[0][0]:.0f}s (% of one core; Pi 5 = 400%), peak threads {peak_threads}")
    for k, v in groups.items():
        print(f"  {k:12s} avg {sum(v) / len(v):5.1f}%  peak {max(v):5.1f}%")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/pi-shots/cpu.txt")
