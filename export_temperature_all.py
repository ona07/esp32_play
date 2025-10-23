import os
import csv
import argparse
from typing import Optional, Iterable, Tuple

import psycopg2
from psycopg2.extensions import connection as PGConnection
from supabase import create_client, Client

def export_temperature_all(
    supabase_url: str,
    supabase_key: Optional[str],
    out_path: str = "temperature_all.csv",
    device_id: Optional[str] = None,
    batch: int = 5000,
):
    if supabase_url.startswith(("postgres://", "postgresql://")):
        rows_iter = _iter_postgres_rows(
            dsn=supabase_url,
            device_id=device_id,
            batch=batch,
        )
    else:
        if not supabase_key:
            raise SystemExit("ERROR: Supabase モードでは --key もしくは SUPABASE_KEY が必要です。")
        rows_iter = _iter_supabase_rows(
            supabase_url=supabase_url,
            supabase_key=supabase_key,
            device_id=device_id,
            batch=batch,
        )

    total = 0
    header_written = False
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        for chunk in rows_iter:
            if not chunk:
                break
            if not header_written:
                writer.writerow(["ts", "device_id", "value"])
                header_written = True
            for r in chunk:
                writer.writerow([r[0], r[1], r[2]])
            total += len(chunk)
            print(f"\rExported: {total}", end="")

    print(f"\nDone. Rows: {total} -> {out_path}")


def _iter_supabase_rows(
    supabase_url: str,
    supabase_key: str,
    device_id: Optional[str],
    batch: int,
) -> Iterable[Tuple[object, object, object]]:
    supabase: Client = create_client(supabase_url, supabase_key)

    q = (
        supabase.table("measurements")
        .select("ts,device_id,value")
        .eq("metric", "temperature")
        .order("ts", {"ascending": True})
    )
    if device_id:
        q = q.eq("device_id", device_id)

    start = 0
    while True:
        page = q.range(start, start + batch - 1).execute()
        rows = page.data or []
        if not rows:
            break
        yield [(r.get("ts"), r.get("device_id"), r.get("value")) for r in rows]
        start += batch


def _iter_postgres_rows(
    dsn: str,
    device_id: Optional[str],
    batch: int,
) -> Iterable[Tuple[object, object, object]]:
    conn: PGConnection = psycopg2.connect(dsn)
    query = (
        "SELECT ts, device_id, value FROM measurements "
        "WHERE metric = %s "
        + ("AND device_id = %s " if device_id else "")
        + "ORDER BY ts"
    )
    params = ["temperature"]
    if device_id:
        params.append(device_id)

    try:
        with conn, conn.cursor(name="temperature_export_cursor") as cur:
            cur.itersize = batch
            cur.execute(query, params)
            while True:
                rows = cur.fetchmany(batch)
                if not rows:
                    break
                yield rows
    finally:
        conn.close()

def main():
    parser = argparse.ArgumentParser(description="Export ALL temperature rows from Supabase/PostgreSQL to CSV (streaming).")
    parser.add_argument(
        "--url",
        default=os.getenv("SUPABASE_URL"),
        help="Supabase project URL (https://...) もしくは PostgreSQL 接続文字列",
    )
    parser.add_argument(
        "--key",
        default=os.getenv("SUPABASE_KEY"),
        help="Supabase anon/service key（Supabase モードのみ必要）",
    )
    parser.add_argument("--out", default="temperature_all.csv", help="Output CSV path")
    parser.add_argument("--device-id", default=None, help="Filter by device_id (optional)")
    parser.add_argument("--batch", type=int, default=5000, help="Fetch batch size")
    args = parser.parse_args()

    if not args.url:
        raise SystemExit("ERROR: --url もしくは SUPABASE_URL を指定してください。")
    if not args.url.startswith(("postgres://", "postgresql://")) and not args.key:
        raise SystemExit("ERROR: Supabase を使う場合は --key もしくは SUPABASE_KEY を指定してください。")

    export_temperature_all(
        supabase_url=args.url,
        supabase_key=args.key,
        out_path=args.out,
        device_id=args.device_id,
        batch=args.batch,
    )

if __name__ == "__main__":
    main()
