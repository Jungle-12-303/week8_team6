#!/usr/bin/env python3
"""
Generate the README load-test trend chart from raw 100-request logs.

Presentation point:
- /health does not enter the DB engine, so it stays almost flat.
- /query enters the worker -> mutex -> SQL engine path, so completion-order
  latency grows as requests wait in front of the serialized DB section.
"""

from __future__ import annotations

import math
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.font_manager as font_manager
import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[1]
HEALTH_LOG = ROOT / "docs" / "load-tests" / "health-100.txt"
QUERY_LOG = ROOT / "docs" / "load-tests" / "query-100-times.txt"
OUTPUT = ROOT / "docs" / "assets" / "load-test-trend.png"


def configure_korean_font() -> None:
    """Use a Korean-capable font when the current OS provides one."""

    candidates = [
        # Windows
        Path("C:/Windows/Fonts/malgun.ttf"),
        # macOS
        Path("/System/Library/Fonts/AppleSDGothicNeo.ttc"),
        Path("/System/Library/Fonts/Supplemental/AppleGothic.ttf"),
        # Common Linux fonts
        Path("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    ]

    for candidate in candidates:
        if not candidate.exists():
            continue
        font_manager.fontManager.addfont(str(candidate))
        font_name = font_manager.FontProperties(fname=str(candidate)).get_name()
        plt.rcParams["font.family"] = font_name
        break

    plt.rcParams["axes.unicode_minus"] = False


def read_totals(path: Path) -> tuple[list[int], list[float]]:
    """Read request ids and total seconds from `req=... code=... total=...` logs."""

    req_ids: list[int] = []
    totals: list[float] = []
    pattern = re.compile(r"req=(\d+)\s+code=(\d+)\s+total=([0-9.]+)")

    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.search(line)
        if not match:
            continue
        req_id = int(match.group(1))
        code = int(match.group(2))
        total = float(match.group(3))
        if code == 200:
            req_ids.append(req_id)
            totals.append(total)

    return req_ids, totals


def percentile(values: list[float], ratio: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(len(ordered) * ratio) - 1)
    return ordered[index]


def draw_summary_box(fig: plt.Figure, x: float, y: float, title: str, metric: str, body: str, color: str, face: str) -> None:
    fig.text(
        x,
        y,
        f"{title}\n{metric}\n{body}",
        ha="left",
        va="top",
        fontsize=13,
        color="#172033",
        linespacing=1.55,
        bbox={
            "boxstyle": "round,pad=0.8,rounding_size=0.18",
            "facecolor": face,
            "edgecolor": color,
            "linewidth": 1.8,
        },
    )


def main() -> None:
    configure_korean_font()

    _, health = read_totals(HEALTH_LOG)
    _, query = read_totals(QUERY_LOG)
    if not health or not query:
        raise SystemExit("load-test logs are empty or invalid")

    order_health = list(range(1, len(health) + 1))
    order_query = list(range(1, len(query) + 1))

    health_avg_ms = sum(health) / len(health) * 1000
    health_p95_ms = percentile(health, 0.95) * 1000
    query_avg = sum(query) / len(query)

    fig, ax = plt.subplots(figsize=(14, 8), dpi=160)
    fig.patch.set_facecolor("#f8fafc")
    ax.set_facecolor("#ffffff")

    fig.suptitle(
        "100회 요청 로그: 평균보다 중요한 것은 완료 순서별 누적 대기",
        x=0.06,
        y=0.965,
        ha="left",
        fontsize=23,
        fontweight="bold",
        color="#172033",
    )
    fig.text(
        0.06,
        0.91,
        "/health는 DB를 거치지 않아 거의 0초에 붙고, /query는 mutex 보호 SQL 구간 앞에서 완료 순서가 뒤로 갈수록 total이 증가한다.",
        ha="left",
        fontsize=12.5,
        color="#475569",
    )

    ax.plot(
        order_health,
        health,
        color="#06b6d4",
        linewidth=3.0,
        marker="o",
        markevery=10,
        markersize=4,
        label="GET /health: DB 미사용, 거의 0초",
        zorder=4,
    )
    ax.plot(
        order_query,
        query,
        color="#f97316",
        linewidth=3.2,
        marker="o",
        markevery=10,
        markersize=4,
        label="POST /query: 뒤로 갈수록 증가",
        zorder=5,
    )

    ax.set_title("완료 순서별 end-to-end total 시간", loc="left", pad=14, fontsize=16, fontweight="bold", color="#172033")
    ax.set_xlabel("완료 순서 (로그에 기록된 순서 1~100)", fontsize=11, color="#334155")
    ax.set_ylabel("클라이언트가 관찰한 total time (seconds)", fontsize=11, color="#334155")
    ax.set_xlim(1, 100)
    ax.set_ylim(0, 35)
    ax.set_xticks([1, 25, 50, 75, 100])
    ax.set_yticks([0, 10, 20, 30])
    ax.grid(True, axis="y", color="#e2e8f0", linewidth=1.0)
    ax.grid(False, axis="x")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color("#94a3b8")
    ax.spines["bottom"].set_color("#94a3b8")
    ax.tick_params(colors="#64748b")
    ax.legend(
        loc="upper left",
        bbox_to_anchor=(0.57, 0.56),
        frameon=True,
        facecolor="white",
        edgecolor="#cbd5e1",
        framealpha=0.96,
        fontsize=10,
    )

    ax.annotate(
        "mutex 보호 SQL 구간 앞에서\n대기 시간이 누적",
        xy=(68, query[67]),
        xytext=(42, 28),
        textcoords="data",
        arrowprops={"arrowstyle": "->", "color": "#334155", "linewidth": 1.8},
        fontsize=10,
        color="#334155",
        ha="left",
        va="center",
    )
    ax.annotate(
        f"첫 완료 {query[0]:.2f}s",
        xy=(1, query[0]),
        xytext=(8, 4.2),
        arrowprops={"arrowstyle": "->", "color": "#f97316", "linewidth": 1.5},
        fontsize=10,
        color="#c2410c",
    )
    ax.annotate(
        f"마지막 완료 {query[-1]:.2f}s",
        xy=(100, query[-1]),
        xytext=(73, 33.7),
        arrowprops={"arrowstyle": "->", "color": "#f97316", "linewidth": 1.5},
        fontsize=10,
        color="#c2410c",
    )

    plt.subplots_adjust(left=0.08, right=0.96, top=0.80, bottom=0.30)

    draw_summary_box(
        fig,
        0.08,
        0.19,
        "GET /health 100회",
        f"100/100 성공 · 평균 {health_avg_ms:.2f}ms · p95 {health_p95_ms:.2f}ms",
        "DB 엔진을 타지 않으므로 Thread Pool/SQL mutex 대기 영향이 거의 없다.",
        "#67e8f9",
        "#ecfeff",
    )
    draw_summary_box(
        fig,
        0.54,
        0.19,
        "POST /query 100회",
        f"100/100 성공 · 첫 완료 {query[0]:.2f}s → 마지막 완료 {query[-1]:.2f}s",
        f"평균 {query_avg:.2f}s는 보조 지표. 핵심은 완료 순서가 뒤로 갈수록 증가한다는 점이다.",
        "#fdba74",
        "#fff7ed",
    )

    fig.text(
        0.08,
        0.045,
        "원본 로그: docs/load-tests/health-100.txt, docs/load-tests/query-100-times.txt",
        fontsize=9,
        color="#64748b",
    )

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUTPUT, facecolor=fig.get_facecolor())
    plt.close(fig)
    print(OUTPUT)


if __name__ == "__main__":
    main()
