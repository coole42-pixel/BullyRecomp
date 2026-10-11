"""Builds report.md, summary.json and a screenshot contact sheet for a tools/autoplay.ps1 session.

usage: python -I autoplay-report.py <session dir> <BullyRecomp.map>
"""
import bisect
import csv
import hashlib
import json
import os
import re
import sys

session, map_path = sys.argv[1], sys.argv[2]
meta = json.load(open(os.path.join(session, "session.json"), encoding="utf-8-sig"))
boot_s = float(meta.get("BootSeconds", 45))


def read_csv(name):
    path = os.path.join(session, name)
    if not os.path.exists(path):
        return []
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def pct(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, int(q * len(ordered)))], 2)


def resolve(rva):
    pattern = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s")
    symbols = {}
    with open(map_path, errors="replace") as handle:
        for line in handle:
            match = pattern.match(line)
            if match:
                symbols.setdefault(int(match.group(2), 16) - 0x140000000, match.group(1))
    starts = sorted(symbols)
    i = bisect.bisect_right(starts, rva) - 1
    return f"{symbols[starts[i]]}+{rva - starts[i]:#x}" if i >= 0 else f"?{rva:#x}"


# --- Frame pacing ---------------------------------------------------------------------------
per_second = read_csv("perf.csv")
frames = [(float(r["t_s"]), float(r["frame_ms"])) for r in read_csv("perf.csv.frames.csv")]
play = [ms for t, ms in frames if t >= boot_s]
hitches = sorted(((ms, t) for t, ms in frames if t >= boot_s and ms > 50), reverse=True)

stalls, run = [], 0
for row in per_second:
    if float(row["t_s"]) < boot_s:
        continue
    run = run + 1 if float(row["fps"]) < 1 else 0
    if run:
        stalls.append((float(row["t_s"]), run))
longest_stall = max((n for _, n in stalls), default=0)

minutes = {}
for row in per_second:
    t = float(row["t_s"])
    if t < boot_s:
        continue
    bucket = minutes.setdefault(int((t - boot_s) // 60), {"fps": [], "max": [], "ws": [], "cpu": []})
    bucket["fps"].append(float(row["fps"]))
    bucket["max"].append(float(row["max_ms"]))
    bucket["ws"].append(float(row["ws_mb"]))
    bucket["cpu"].append(float(row["cpu_cores"]))

ws = [float(r["ws_mb"]) for r in per_second if float(r["t_s"]) >= boot_s]
ws_growth_per_10min = None
if len(ws) >= 480:
    # Compare the first and last minute so start-up allocations do not count as growth. Shorter
    # sessions are still streaming in the world, so their slope says nothing about leaks.
    first, last = sum(ws[:60]) / 60, sum(ws[-60:]) / 60
    ws_growth_per_10min = round((last - first) / (len(ws) / 600), 1)

# --- Screenshots ----------------------------------------------------------------------------
captures_dir = os.path.join(session, "captures")
shots = []
if os.path.isdir(captures_dir):
    names = [n for n in os.listdir(captures_dir) if re.fullmatch(r"frame-\d+\.ppm", n)]
    names.sort(key=lambda n: int(re.findall(r"\d+", n)[0]))
    interval = float(meta.get("CaptureEverySec", 3))
    try:
        from PIL import Image, ImageDraw, ImageStat
    except ImportError:
        Image = None
    for index, name in enumerate(names):
        path = os.path.join(captures_dir, name)
        entry = {"file": name, "t_s": round((int(re.findall(r"\d+", name)[0]) + 1) * interval)}
        entry["hash"] = hashlib.sha1(open(path, "rb").read()).hexdigest()
        if Image:
            image = Image.open(path).convert("RGB")
            entry["luma"] = round(sum(ImageStat.Stat(image.convert("L")).mean), 1)
            entry["thumb"] = image.resize((320, 180))
        shots.append(entry)
    if shots and Image:
        cols = 6
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * 320, rows * 200), (20, 20, 20))
        draw = ImageDraw.Draw(sheet)
        for i, entry in enumerate(shots):
            x, y = (i % cols) * 320, (i // cols) * 200
            sheet.paste(entry.pop("thumb"), (x, y))
            draw.text((x + 4, y + 182), f"t={entry['t_s']}s  luma={entry['luma']}", fill=(230, 230, 230))
        sheet.save(os.path.join(session, "contact-sheet.png"))

after_boot = [s for s in shots if s["t_s"] >= boot_s]
black_run = frozen_run = best_black = best_frozen = 0
previous_hash = None
for s in after_boot:
    black_run = black_run + 1 if s.get("luma", 255) < 8 else 0
    frozen_run = frozen_run + 1 if s["hash"] == previous_hash else 0
    previous_hash = s["hash"]
    best_black, best_frozen = max(best_black, black_run), max(best_frozen, frozen_run)

# --- Verdict --------------------------------------------------------------------------------
problems = []
if meta.get("Outcome") == "crashed":
    where = f" at {resolve(int(meta['FaultRva'], 16))}" if meta.get("FaultRva") else ""
    problems.append(f"Crashed after {meta.get('RanSeconds')} s (exit {meta.get('ExitCode')}){where}")
if meta.get("Outcome") == "hung":
    problems.append(f"Hung: no guest frames for {meta.get('HangSeconds')} s at t={meta.get('RanSeconds')} s")
if longest_stall >= 5:
    problems.append(f"Longest stall: {longest_stall} s without a frame")
if best_black >= 3:
    problems.append(f"{best_black + 0} consecutive black screenshots after boot")
if best_frozen >= 3:
    problems.append(f"{best_frozen + 1} identical screenshots in a row (frozen image or menu)")
verdict = "FAIL" if problems else "PASS"

summary = {
    "Verdict": verdict,
    "Problems": problems,
    "Session": meta,
    "PlaySeconds": round(max((t for t, _ in frames), default=0) - boot_s, 1),
    "GuestFrames": len(play),
    "AvgFps": round(1000 / (sum(play) / len(play)), 2) if play else None,
    "FrameP50Ms": pct(play, 0.5),
    "FrameP95Ms": pct(play, 0.95),
    "FrameP99Ms": pct(play, 0.99),
    "FramesOver20ms": sum(1 for ms in play if ms > 20),
    "FramesOver50ms": len(hitches),
    "WorstFrameMs": round(max(play), 1) if play else None,
    "LongestStallSeconds": longest_stall,
    "WorkingSetStartMb": ws[0] if ws else None,
    "WorkingSetMaxMb": max(ws) if ws else None,
    "WorkingSetGrowthMbPer10Min": ws_growth_per_10min,
    "Screenshots": len(shots),
}
json.dump(summary, open(os.path.join(session, "summary.json"), "w"), indent=2)

lines = [
    f"# Autoplay session `{meta['Name']}`: **{verdict}**",
    "",
    f"Mode `{meta['Mode']}`, seed {meta.get('Seed')}, {meta.get('RanSeconds')} s run "
    f"({boot_s:.0f} s boot phase), outcome **{meta.get('Outcome')}**, started {meta.get('Started')}.",
    "",
]
if problems:
    lines += ["## Problems", ""] + [f"- {p}" for p in problems] + [""]
lines += [
    "## Frame pacing (after boot)",
    "",
    "| Frames | Avg fps | p50 ms | p95 ms | p99 ms | >20 ms | >50 ms | Worst ms | Longest stall |",
    "|---|---|---|---|---|---|---|---|---|",
    f"| {summary['GuestFrames']} | {summary['AvgFps']} | {summary['FrameP50Ms']} | {summary['FrameP95Ms']} | "
    f"{summary['FrameP99Ms']} | {summary['FramesOver20ms']} | {summary['FramesOver50ms']} | "
    f"{summary['WorstFrameMs']} | {longest_stall} s |",
    "",
    "## Per minute",
    "",
    "| Minute | Avg fps | Min fps | Worst frame ms | CPU cores | Working set MB |",
    "|---|---|---|---|---|---|",
]
for minute, b in sorted(minutes.items()):
    lines.append(
        f"| {minute} | {sum(b['fps']) / len(b['fps']):.1f} | {min(b['fps']):.0f} | {max(b['max']):.0f} | "
        f"{sum(b['cpu']) / len(b['cpu']):.2f} | {max(b['ws']):.0f} |")
lines += ["", f"Working set: start {summary['WorkingSetStartMb']} MB, max {summary['WorkingSetMaxMb']} MB, "
          f"growth {ws_growth_per_10min} MB per 10 min.", ""]
if hitches:
    lines += ["## Worst hitches", "", "| t (s) | Frame ms |", "|---|---|"]
    lines += [f"| {t:.1f} | {ms:.0f} |" for ms, t in hitches[:15]]
    lines.append("")
if shots:
    lines += ["## Screenshots", "", "![contact sheet](contact-sheet.png)", "",
              f"{len(shots)} captures every {meta.get('CaptureEverySec')} s; longest black run "
              f"{best_black}, longest identical run {best_frozen + 1 if best_frozen else 0}.", ""]
open(os.path.join(session, "report.md"), "w", encoding="utf-8").write("\n".join(lines))
print(json.dumps({k: summary[k] for k in ("Verdict", "Problems", "AvgFps", "FrameP95Ms", "FrameP99Ms",
                                          "LongestStallSeconds", "WorkingSetGrowthMbPer10Min")}))
