from fastapi import FastAPI, Header, HTTPException, Query, status
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from typing import List, Optional, Literal, Tuple
import os, datetime as dt, traceback
import psycopg2, psycopg2.extras

# ===================== App & CORS =====================
DATABASE_URL = os.getenv("DATABASE_URL")
DEBUG = os.getenv("DEBUG", "0") == "1"   # デバッグ時のみ 1 にしてください（本番は 0 推奨）

app = FastAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],                 # 必要なら ["https://ona07.github.io"] などに絞ってOK
    allow_methods=["GET", "POST", "OPTIONS"],
    allow_headers=["*"],
)

# ===================== Models =====================
class Measure(BaseModel):
    metric: Literal["temperature","humidity","distance_ultrasonic"]
    value: float
    ts: Optional[str] = None
    meta: dict = Field(default_factory=dict)

# ===================== DB helpers =====================
def conn():
    """
    Render / Supabase 等の SSL 必須環境の保険。
    DATABASE_URL に sslmode が無ければ require を付ける。
    """
    if not DATABASE_URL:
        raise RuntimeError("DATABASE_URL not set")
    if "sslmode=" not in DATABASE_URL:
        return psycopg2.connect(DATABASE_URL, sslmode="require")
    return psycopg2.connect(DATABASE_URL)

def device_id_from_key(api_key: str):
    with conn() as c, c.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("select id from public.devices where api_key=%s", (api_key,))
        r = cur.fetchone()
        return r["id"] if r else None

# ---- ts 列の型に依存しないクエリ生成のための設定 ----
#   - timestamptz の場合:   TS_EXPR="ts",                TS_GUARD=""
#   - text/varchar の場合:  TS_EXPR="(ts::timestamptz)", TS_GUARD=" AND (ts::text) ~* %s"  (ISO 正規表現)
TS_EXPR: str = "ts"
TS_GUARD: str = ""     # 追加 WHERE 句（先頭にスペース＋ANDを含む）
TS_IS_TEXT: bool = False
REGEX_ISO = r'^[0-9]{4}-[0-9]{2}-[0-9]{2}[T ][0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?([+-][0-9]{2}:[0-9]{2}|Z)?$'

def detect_ts_conf() -> Tuple[str, str, bool]:
    """
    information_schema から public.measurements.ts の data_type を検出。
    """
    global TS_EXPR, TS_GUARD, TS_IS_TEXT
    with conn() as c, c.cursor() as cur:
        cur.execute("""
            select data_type
            from information_schema.columns
            where table_schema='public' and table_name='measurements' and column_name='ts'
            limit 1
        """)
        row = cur.fetchone()
        dtype = row[0] if row else None
        if dtype and ("time" in dtype and "zone" in dtype):
            TS_EXPR = "ts"
            TS_GUARD = ""
            TS_IS_TEXT = False
        else:
            TS_EXPR = "(ts::timestamptz)"
            TS_GUARD = " AND (ts::text) ~* %s"
            TS_IS_TEXT = True
    print(f"[BOOT] ts column detected: dtype={dtype}, TS_EXPR={TS_EXPR}, TS_IS_TEXT={TS_IS_TEXT}")
    return TS_EXPR, TS_GUARD, TS_IS_TEXT

@app.on_event("startup")
def _on_startup():
    try:
        detect_ts_conf()
    except Exception as e:
        print("[WARN] ts detection failed:", repr(e))
        traceback.print_exc()

def http503(e: Exception, where: str):
    # ログに詳細
    print(f"[SERVE][ERROR][{where}] {repr(e)}")
    traceback.print_exc()
    # デバッグ時のみ本文にエラー詳細を含める（本番では固定文言）
    if DEBUG:
        raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, f"{where}: {type(e).__name__}: {e}")
    raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "temporarily unavailable")

# ===================== Health / Schema =====================
@app.get("/healthz")
def healthz():
    try:
        with conn() as c, c.cursor() as cur:
            cur.execute("select 1")
            cur.fetchone()
        return {"ok": True}
    except Exception as e:
        print("[HEALTHZ][ERROR]", repr(e))
        traceback.print_exc()
        return {"ok": False, "error": str(e)}

@app.get("/schemaz")
def schemaz(device_id: Optional[str] = None):
    """
    テーブル存在や ts 型、（任意）デバイス別の件数などを確認。
    device_id は整数でもUUIDでもOK（::text 比較）。
    """
    out = {}
    try:
        with conn() as c, c.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute("select to_regclass('public.devices') as devices, to_regclass('public.measurements') as measurements;")
            tables = cur.fetchone()
            out["tables"] = tables
            cur.execute("""
                select data_type
                from information_schema.columns
                where table_schema='public' and table_name='measurements' and column_name='ts' limit 1
            """)
            row = cur.fetchone()
            out["ts_type"] = row["data_type"] if row else None
            out["ts_expr"] = TS_EXPR
            out["ts_is_text"] = TS_IS_TEXT
            if device_id is not None and tables and tables.get("measurements"):
                cur.execute("""
                    select metric, count(*) cnt
                    from public.measurements
                    where device_id::text = %s
                    group by 1 order by 1
                """, (device_id,))
                out["counts"] = cur.fetchall()
        return {"ok": True, **out}
    except Exception as e:
        http503(e, "schemaz")

# ===================== Ingest =====================
@app.post("/ingest")
def ingest(measures: List[Measure], x_api_key: str = Header(default="")):
    try:
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
    except HTTPException:
        raise
    except Exception as e:
        http503(e, "ingest")

# ===================== Latest =====================
# 例:
#   /latest?device_id=232928c2-22a1-498d-aec0-71d0a48ee43a&metric=temperature
#   /latest?device_id=232928c2-22a1-498d-aec0-71d0a48ee43a   ← 全メトリクスの最新1件ずつ
@app.get("/latest")
def get_latest(
    device_id: str = Query(..., description="devices.id（整数 or UUID）"),
    metric: Optional[Literal["temperature","humidity","distance_ultrasonic"]] = Query(
        None, description="指定時はそのメトリクスだけの最新値"
    ),
):
    try:
        with conn() as c, c.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            guard = TS_GUARD if TS_IS_TEXT else ""
            if metric:
                sql = f"""
                    SELECT device_id, {TS_EXPR} AS ts, metric, value, meta
                    FROM public.measurements
                    WHERE device_id::text = %s AND metric = %s
                    {guard}
                    ORDER BY {TS_EXPR} DESC
                    LIMIT 1
                """
                params = [device_id, metric]
                if TS_IS_TEXT: params.append(REGEX_ISO)
                cur.execute(sql, params)
                row = cur.fetchone()
                if not row:
                    return {"ok": True, "data": None}
                if isinstance(row["ts"], dt.datetime):
                    row["ts"] = row["ts"].isoformat()
                return {"ok": True, "data": row}
            else:
                sql = f"""
                    SELECT DISTINCT ON (metric)
                           device_id, {TS_EXPR} AS ts, metric, value, meta
                    FROM public.measurements
                    WHERE device_id::text = %s
                    {guard}
                    ORDER BY metric, {TS_EXPR} DESC
                """
                params = [device_id]
                if TS_IS_TEXT: params.append(REGEX_ISO)
                cur.execute(sql, params)
                rows = cur.fetchall()
                for r in rows:
                    if isinstance(r["ts"], dt.datetime):
                        r["ts"] = r["ts"].isoformat()
                return {"ok": True, "data": rows}
    except HTTPException:
        raise
    except Exception as e:
        http503(e, "latest")

# ===================== Series =====================
# 例:
#   /series?device_id=232928c2-22a1-498d-aec0-71d0a48ee43a&metric=temperature&start=2025-08-31T00:00:00Z&end=2025-08-31T12:00:00Z
#   /series?device_id=232928c2-22a1-498d-aec0-71d0a48ee43a&start=2025-08-30T00:00:00+09:00&end=2025-08-31T00:00:00+09:00
@app.get("/series")
def get_series(
    device_id: str = Query(..., description="devices.id（整数 or UUID）"),
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
    try:
        if end is None:
            end = dt.datetime.now(dt.timezone.utc)
        if start is None:
            start = end - dt.timedelta(days=1)
        if start.tzinfo is None:
            start = start.replace(tzinfo=dt.timezone.utc)
        if end.tzinfo is None:
            end = end.replace(tzinfo=dt.timezone.utc)

        guard = TS_GUARD if TS_IS_TEXT else ""
        if metric:
            # device_id → (ガード) → 範囲 → metric の順でパラメータを積む
            sql = f"""
                SELECT device_id, {TS_EXPR} AS ts, metric, value, meta
                FROM public.measurements
                WHERE device_id::text = %s
                  {guard}
                  AND {TS_EXPR} >= %s
                  AND {TS_EXPR} <  %s
                  AND metric = %s
                ORDER BY {TS_EXPR} ASC
            """
            params: List = [device_id]
            if TS_IS_TEXT: params.append(REGEX_ISO)
            params.extend([start, end, metric])
        else:
            sql = f"""
                SELECT device_id, {TS_EXPR} AS ts, metric, value, meta
                FROM public.measurements
                WHERE device_id::text = %s
                  {guard}
                  AND {TS_EXPR} >= %s
                  AND {TS_EXPR} <  %s
                ORDER BY {TS_EXPR} ASC, metric ASC
            """
            params = [device_id]
            if TS_IS_TEXT: params.append(REGEX_ISO)
            params.extend([start, end])

        if limit:
            sql += " LIMIT %s"
            params.append(limit)

        with conn() as c, c.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, params)
            rows = cur.fetchall()
            for r in rows:
                if isinstance(r["ts"], dt.datetime):
                    r["ts"] = r["ts"].isoformat()
            return {"ok": True, "count": len(rows), "data": rows}
    except HTTPException:
        raise
    except Exception as e:
        http503(e, "series")

# ===================== Root =====================
@app.get("/")
def root():
    return {"ok": True, "service": "esp32-play", "version": 1}
