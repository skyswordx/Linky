#!/usr/bin/env python3
"""
Run Linky learning labs and generate a single-file HTML report.

The script intentionally uses only Python's standard library. It should work on
fresh Linux boxes after the C project is built, without pandas or matplotlib.
"""

from __future__ import annotations

import argparse
import csv
import html
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


CSV_FIELDS = [
    "mode",
    "frame_size",
    "buffers",
    "iterations",
    "eventfd",
    "mlockall",
    "producer_cpu",
    "consumer_cpu",
    "rt_prio",
    "samples",
    "dropped",
    "elapsed_ms",
    "avg_us",
    "p50_us",
    "p95_us",
    "p99_us",
    "max_us",
    "lab",
    "case",
    "note",
    "status",
    "stderr",
]


@dataclass(frozen=True)
class LabCase:
    lab: str
    case: str
    note: str
    args: tuple[str, ...]
    needs_root: bool = False


def build_cases(include_dmabuf: bool) -> list[LabCase]:
    cases: list[LabCase] = []

    for size in ("4K", "64K", "1M", "4M"):
        cases.append(
            LabCase(
                lab="frame_size",
                case=size,
                note="larger frames touch more cache lines; this is the CPU-cost intuition behind offloading resize/color conversion",
                args=("pool", "--frames", "10000", "--frame-size", size, "--buffers", "8", "--ring", "64"),
            )
        )

    for mode in ("pool", "memfd"):
        cases.append(
            LabCase(
                lab="backend",
                case=mode,
                note="same descriptor pipeline, different backing store; fd-backed memory alone is not acceleration",
                args=(mode, "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64"),
            )
        )

    if include_dmabuf:
        cases.append(
            LabCase(
                lab="backend",
                case="dmabuf",
                note="dma-buf heap validates fd allocation and CPU access sync, not real device import",
                args=("dmabuf", "--heap", "/dev/dma_heap/system", "--frames", "5000", "--frame-size", "1M", "--buffers", "8", "--ring", "64"),
                needs_root=True,
            )
        )

    cases.extend(
        [
            LabCase(
                lab="notify",
                case="eventfd",
                note="sleep/wakeup ready notification; fence-like for teaching, not a dma-fence",
                args=("pool", "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64"),
            ),
            LabCase(
                lab="notify",
                case="busy_yield",
                note="polling-style wait; may reduce wakeup cost but burns CPU and can disturb the system",
                args=("pool", "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64", "--no-eventfd"),
            ),
        ]
    )

    for buffers in ("2", "4", "8", "16"):
        cases.append(
            LabCase(
                lab="buffers",
                case=buffers,
                note="small pools block quickly; large pools allow more in-flight data and can hide latency buildup",
                args=("pool", "--frames", "20000", "--frame-size", "1M", "--buffers", buffers, "--ring", "64"),
            )
        )

    cases.extend(
        [
            LabCase(
                lab="scheduling",
                case="baseline",
                note="baseline without memory locking or CPU affinity",
                args=("memfd", "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64"),
            ),
            LabCase(
                lab="scheduling",
                case="mlockall",
                note="locks mappings to reduce page-fault jitter when permitted",
                args=("memfd", "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64", "--mlockall"),
            ),
        ]
    )

    if (os.cpu_count() or 0) >= 4:
        cases.append(
            LabCase(
                lab="scheduling",
                case="affinity_2_3",
                note="producer and consumer pinned to different CPUs; compare p99/max against baseline",
                args=("memfd", "--frames", "20000", "--frame-size", "1M", "--buffers", "8", "--ring", "64", "--mlockall", "--producer-cpu", "2", "--consumer-cpu", "3"),
            )
        )

    return cases


def parse_csv_row(stdout: str) -> dict[str, str]:
    rows = list(csv.DictReader([",".join(CSV_FIELDS[:17]), stdout.strip()]))
    if len(rows) != 1:
        raise ValueError(f"expected one CSV row, got {len(rows)}")
    return dict(rows[0])


def run_case(exe: Path, case: LabCase, use_sudo_for_dmabuf: bool) -> dict[str, str]:
    cmd = [str(exe), *case.args, "--csv"]
    if case.needs_root and use_sudo_for_dmabuf and os.geteuid() != 0:
        cmd = ["sudo", *cmd]

    try:
        proc = subprocess.run(
            cmd,
            check=False,
            text=True,
            capture_output=True,
        )
    except FileNotFoundError as exc:
        row = empty_row(case)
        row["status"] = "failed"
        row["stderr"] = str(exc)
        return row

    if proc.returncode != 0:
        row = empty_row(case)
        row["status"] = f"failed:{proc.returncode}"
        row["stderr"] = proc.stderr.strip().replace("\n", " | ")
        return row

    row = parse_csv_row(proc.stdout)
    row["lab"] = case.lab
    row["case"] = case.case
    row["note"] = case.note
    row["status"] = "ok"
    row["stderr"] = ""
    return row


def empty_row(case: LabCase) -> dict[str, str]:
    row = {field: "" for field in CSV_FIELDS}
    row["lab"] = case.lab
    row["case"] = case.case
    row["note"] = case.note
    return row


def write_csv(path: Path, rows: Iterable[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def numeric(row: dict[str, str], field: str) -> float:
    try:
        return float(row.get(field, "") or 0.0)
    except ValueError:
        return 0.0


def rows_by_lab(rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault(row["lab"], []).append(row)
    return grouped


def bar(value: float, max_value: float, label: str, color: str) -> str:
    width = 0 if max_value <= 0 else max(2, int((value / max_value) * 260))
    return (
        f'<div class="bar-row"><span class="bar-label">{html.escape(label)}</span>'
        f'<div class="bar-bg"><div class="bar" style="width:{width}px;background:{color}"></div></div>'
        f'<span class="bar-value">{value:.2f} us</span></div>'
    )


def render_lab(lab: str, rows: list[dict[str, str]]) -> str:
    ok_rows = [row for row in rows if row["status"] == "ok"]
    max_p99 = max((numeric(row, "p99_us") for row in ok_rows), default=0.0)
    max_max = max((numeric(row, "max_us") for row in ok_rows), default=0.0)

    pieces = [f"<section><h2>{html.escape(lab)}</h2>"]
    pieces.append("<p>" + html.escape(lab_hint(lab)) + "</p>")

    for row in rows:
        title = f'{row["case"]} / {row["mode"] or "n/a"}'
        pieces.append(f'<article class="case"><h3>{html.escape(title)}</h3>')
        pieces.append(f'<p class="note">{html.escape(row["note"])}</p>')
        if row["status"] != "ok":
            pieces.append(f'<p class="fail">status={html.escape(row["status"])} {html.escape(row["stderr"])}</p>')
        else:
            pieces.append(bar(numeric(row, "p99_us"), max_p99, "p99", "#3b82f6"))
            pieces.append(bar(numeric(row, "max_us"), max_max, "max", "#ef4444"))
            pieces.append(
                '<p class="meta">'
                f'avg={html.escape(row["avg_us"])}us, '
                f'p50={html.escape(row["p50_us"])}us, '
                f'p95={html.escape(row["p95_us"])}us, '
                f'dropped={html.escape(row["dropped"])}</p>'
            )
        pieces.append("</article>")

    pieces.append("</section>")
    return "\n".join(pieces)


def lab_hint(lab: str) -> str:
    hints = {
        "frame_size": "Build the intuition that CPU cost grows when the hot path touches more cache lines from large frames.",
        "backend": "Compare memory object semantics. Similar latency means fd-backed memory is not acceleration by itself.",
        "notify": "Compare sleep/wakeup notification with busy-yield polling. Watch p99/max, not only avg.",
        "buffers": "Queue depth controls backpressure and in-flight latency. More buffers are not automatically better.",
        "scheduling": "Look for tail-latency changes from mlockall and CPU affinity.",
    }
    return hints.get(lab, "Compare p99 and max across cases.")


def render_html(rows: list[dict[str, str]], csv_path: Path) -> str:
    grouped = rows_by_lab(rows)
    sections = "\n".join(render_lab(lab, grouped[lab]) for lab in grouped)
    table_rows = "\n".join(
        "<tr>"
        + "".join(f"<td>{html.escape(row.get(field, ''))}</td>" for field in CSV_FIELDS)
        + "</tr>"
        for row in rows
    )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Linky Learning Labs Report</title>
<style>
body {{ font-family: system-ui, -apple-system, Segoe UI, sans-serif; margin: 32px; color: #17202a; }}
h1 {{ margin-bottom: 0; }}
.sub {{ color: #5b6472; margin-top: 6px; }}
section {{ margin-top: 32px; border-top: 1px solid #d9dee7; padding-top: 20px; }}
.case {{ padding: 12px 0 18px; border-bottom: 1px solid #edf0f5; }}
.case h3 {{ margin-bottom: 4px; }}
.note, .meta {{ color: #4b5563; }}
.fail {{ color: #b91c1c; background: #fee2e2; padding: 8px; border-radius: 6px; }}
.bar-row {{ display: grid; grid-template-columns: 56px 280px 120px; align-items: center; gap: 8px; margin: 6px 0; }}
.bar-label {{ font-weight: 600; }}
.bar-bg {{ width: 260px; height: 14px; background: #eef2f7; border-radius: 7px; overflow: hidden; }}
.bar {{ height: 14px; }}
.bar-value {{ font-variant-numeric: tabular-nums; }}
table {{ border-collapse: collapse; width: 100%; margin-top: 24px; font-size: 12px; }}
th, td {{ border: 1px solid #d9dee7; padding: 4px 6px; text-align: left; }}
th {{ background: #f6f8fb; }}
</style>
</head>
<body>
<h1>Linky Learning Labs Report</h1>
<p class="sub">Raw CSV: {html.escape(str(csv_path))}. Focus on p99 and max first; avg is secondary for low-latency intuition.</p>
<p>This report is a learning artifact for KyLink-style video inference pipelines. It compares controlled variables in a minimal Linux C lab: frame size, backing store, notification style, buffer count, and scheduling hints.</p>
{sections}
<section>
<h2>Raw Rows</h2>
<table>
<thead><tr>{''.join(f'<th>{html.escape(field)}</th>' for field in CSV_FIELDS)}</tr></thead>
<tbody>
{table_rows}
</tbody>
</table>
</section>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Linky learning labs and generate HTML report.")
    parser.add_argument("--exe", default="./build/linky", help="Path to the built linky executable.")
    parser.add_argument("--out-dir", default="reports", help="Output directory.")
    parser.add_argument("--sudo-dmabuf", action="store_true", help="Use sudo for the dmabuf case if not root.")
    parser.add_argument("--skip-dmabuf", action="store_true", help="Skip DMA-BUF case.")
    args = parser.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"missing executable: {exe}", file=sys.stderr)
        return 2

    if args.sudo_dmabuf and shutil.which("sudo") is None:
        print("sudo requested but not found", file=sys.stderr)
        return 2

    include_dmabuf = not args.skip_dmabuf and Path("/dev/dma_heap/system").exists()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "linky_learning_labs.csv"
    html_path = out_dir / "linky_learning_labs.html"

    rows = []
    for case in build_cases(include_dmabuf):
        print(f"[{case.lab}] {case.case}")
        rows.append(run_case(exe, case, args.sudo_dmabuf))

    write_csv(csv_path, rows)
    html_path.write_text(render_html(rows, csv_path), encoding="utf-8")
    print(f"wrote {csv_path}")
    print(f"wrote {html_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
