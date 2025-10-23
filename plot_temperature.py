import argparse
import csv
import datetime as dt
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt


def parse_timestamp(value: str) -> dt.datetime:
    """Supabase/ISO 8601 -> timezone-aware datetime."""
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    return dt.datetime.fromisoformat(value)


def load_rows(
    csv_path: Path,
    device_filter: Optional[str],
    target_date: Optional[dt.date],
) -> Dict[str, List[Tuple[dt.datetime, float]]]:
    """Read CSV rows and group by device_id."""
    grouped: Dict[str, List[Tuple[dt.datetime, float]]] = defaultdict(list)
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            device_id = row.get("device_id") or "unknown"
            if device_filter and device_id != device_filter:
                continue

            ts_raw = row.get("ts")
            value_raw = row.get("value")
            if not ts_raw or value_raw is None:
                continue

            try:
                timestamp = parse_timestamp(ts_raw)
                value = float(value_raw)
            except (ValueError, TypeError):
                continue

            if target_date and timestamp.date() != target_date:
                continue

            grouped[device_id].append((timestamp, value))

    for device_rows in grouped.values():
        device_rows.sort(key=lambda pair: pair[0])

    return grouped


def plot(grouped: Dict[str, List[Tuple[dt.datetime, float]]], title: str) -> None:
    if not grouped:
        raise SystemExit("データがありません（フィルタ条件を確認してください）。")

    plt.figure(figsize=(12, 6))
    for device_id, rows in grouped.items():
        timestamps, values = zip(*rows)
        plt.plot(timestamps, values, marker="o", linewidth=1, label=device_id)

    plt.title(title)
    plt.xlabel("timestamp")
    plt.ylabel("temperature")
    plt.grid(True, linestyle="--", alpha=0.4)
    if len(grouped) > 1:
        plt.legend()
    plt.tight_layout()
    plt.show()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="temperature_all.csv を可視化する簡易グラフ表示スクリプト",
    )
    parser.add_argument(
        "--csv",
        default="temperature_all.csv",
        help="読み込む CSV ファイルパス（デフォルト: temperature_all.csv）",
    )
    parser.add_argument(
        "--device-id",
        help="特定の device_id のみを表示したい場合に指定",
    )
    parser.add_argument(
        "--date",
        help="YYYY-MM-DD 形式で指定すると、その1日分に限定して表示",
    )
    parser.add_argument(
        "--title",
        default="Temperature measurements",
        help="グラフタイトル",
    )
    args = parser.parse_args()

    csv_path = Path(args.csv)
    if not csv_path.exists():
        raise SystemExit(f"CSV が見つかりません: {csv_path}")

    target_date: Optional[dt.date] = None
    if args.date:
        try:
            target_date = dt.date.fromisoformat(args.date)
        except ValueError:
            raise SystemExit("ERROR: --date は YYYY-MM-DD 形式で指定してください。")

    grouped = load_rows(csv_path, args.device_id, target_date)
    plot(grouped, args.title)


if __name__ == "__main__":
    main()
