from fastapi import FastAPI, Header, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field            # ← これが必要！
from typing import List, Optional, Literal
import os, datetime as dt
import psycopg2, psycopg2.extras

DATABASE_URL = os.getenv("DATABASE_URL")
app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],            # GitHub Pages など複数 Origin から叩くなら *
    allow_methods=["GET", "POST", "OPTIONS"],
    allow_headers=["*"],
    allow_credentials=False 
)


class Measure(BaseModel):
    metric: Literal["temperature","humidity","distance_ultrasonic"]
    value: float
    ts: Optional[str] = None
    meta: dict = Field(default_factory=dict)

def conn():
    return psycopg2.connect(DATABASE_URL)

def device_id_from_key(api_key: str):
    with conn() as c, c.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("select id from public.devices where api_key=%s", (api_key,))
        r = cur.fetchone()
        return r["id"] if r else None

# ========= 既存: Ingest =========
@app.post("/ingest")
def ingest(measures: List[Measure], x_api_key: str = Header(default="")):
    if not x_api_key:
        raise HTTPException(401, "Missing X-API-Key")
    did = device_id_from_key(x_api_key)
    if not did:
        raise HTTPException(403, "Invalid API key")

    now = dt.datetime.utcnow().isoformat() + "Z"
    rows = []
    for m in measures:
        ts = m.ts or now
        rows.append((did, ts, m.metric, m.value, psycopg2.extras.Json(dict(m.meta))))
    if not rows:
        return {"ok": True, "inserted": 0}

    with conn() as c, c.cursor() as cur:
        psycopg2.extras.execute_values(
            cur,
            "insert into public.measurements (device_id, ts, metric, value, meta) values %s",
            rows
        )
    return {"ok": True, "inserted": len(rows)}

# ========= 追加: 最新値取得 =========
# 例:
#   /latest?device_id=1&metric=temperature
#   /latest?device_id=1   ← 全メトリクスの最新1件ずつ
@app.get("/latest")
def get_latest(
    device_id: int = Query(..., description="devices.id"),
    metric: Optional[Literal["temperature","humidity","distance_ultrasonic"]] = Query(
        None, description="指定時はそのメトリクスだけの最新値"
    ),
):
    with conn() as c, c.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
        if metric:
            cur.execute(
                """
                SELECT device_id, (ts::timestamptz) AS ts, metric, value, meta
                FROM public.measurements
                WHERE device_id = %s AND metric = %s
                ORDER BY (ts::timestamptz) DESC
                LIMIT 1
                """,
                (device_id, metric),
            )
            row = cur.fetchone()
            if not row:
                return {"ok": True, "data": None}
            row["ts"] = row["ts"].isoformat()
            return {"ok": True, "data": row}
        else:
            cur.execute(
                """
                SELECT DISTINCT ON (metric)
                       device_id, (ts::timestamptz) AS ts, metric, value, meta
                FROM public.measurements
                WHERE device_id = %s
                ORDER BY metric, (ts::timestamptz) DESC
                """,
                (device_id,),
            )
            rows = cur.fetchall()
            for r in rows:
                r["ts"] = r["ts"].isoformat()
            return {"ok": True, "data": rows}

# ========= 追加: 期間の時系列取得 =========
# 例:
#   /series?device_id=1&metric=temperature&start=2025-08-31T00:00:00Z&end=2025-08-31T12:00:00Z
#   /series?device_id=1&start=2025-08-30T00:00:00+09:00&end=2025-08-31T00:00:00+09:00
@app.get("/series")
def get_series(
    device_id: int = Query(..., description="devices.id"),
    metric: Optional[Literal["temperature","humidity","distance_ultrasonic"]] = Query(
        None, description="指定時はそのメトリクスのみ"
    ),
    start: Optional[dt.datetime] = Query(
        None, description="ISO8601（Z も +09:00 も可）。未指定は end-24h"
    ),
    end: Optional[dt.datetime] = Query(
        None, description="ISO8601（Z も +09:00 も可）。未指定は now()"
    ),
    limit: Optional[int] = Query(
        None, ge=1, le=200000, description="最大件数（任意）。未指定は制限なし"
    ),
):
    if end is None:
        end = dt.datetime.now(dt.timezone.utc)
    if start is None:
        start = end - dt.timedelta(days=1)
    if start.tzinfo is None:
        start = start.replace(tzinfo=dt.timezone.utc)
    if end.tzinfo is None:
        end = end.replace(tzinfo=dt.timezone.utc)

    params = [device_id, start, end]
    if metric:
        sql = """
            SELECT device_id, (ts::timestamptz) AS ts, metric, value, meta
            FROM public.measurements
            WHERE device_id = %s
              AND (ts::timestamptz) >= %s
              AND (ts::timestamptz) <  %s
              AND metric = %s
            ORDER BY (ts::timestamptz) ASC
        """
        params = [device_id, start, end, metric]
    else:
        sql = """
            SELECT device_id, (ts::timestamptz) AS ts, metric, value, meta
            FROM public.measurements
            WHERE device_id = %s
              AND (ts::timestamptz) >= %s
              AND (ts::timestamptz) <  %s
            ORDER BY (ts::timestamptz) ASC, metric ASC
        """
    if limit:
        sql += " LIMIT %s"
        params.append(limit)

    with conn() as c, c.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
        cur.execute(sql, params)
        rows = cur.fetchall()
        for r in rows:
            r["ts"] = r["ts"].isoformat()
        return {"ok": True, "count": len(rows), "data": rows}
