from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel, Field
from typing import List, Optional, Literal
import os, datetime as dt
import psycopg2, psycopg2.extras

DATABASE_URL = os.getenv("DATABASE_URL")
app = FastAPI()

class Measure(BaseModel):
    metric: Literal["temperature","humidity","distance_ultrasonic"]
    value: float
    ts: Optional[str] = None
    meta: dict = Field(default_factory=dict)

def conn(): return psycopg2.connect(DATABASE_URL)

def device_id_from_key(api_key:str):
    with conn() as c, c.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
        cur.execute("select id from public.devices where api_key=%s", (api_key,))
        r = cur.fetchone(); return r["id"] if r else None

@app.post("/ingest")
def ingest(measures: List[Measure], x_api_key: str = Header(default="")):
    if not x_api_key: raise HTTPException(401, "Missing X-API-Key")
    did = device_id_from_key(x_api_key)
    if not did: raise HTTPException(403, "Invalid API key")
    now = dt.datetime.utcnow().isoformat() + "Z"
    rows = []
    for m in measures:
        ts = m.ts or now
        rows.append((did, ts, m.metric, m.value, psycopg2.extras.Json(dict(m.meta))))
    with conn() as c, c.cursor() as cur:
        psycopg2.extras.execute_values(cur,
            "insert into public.measurements (device_id, ts, metric, value, meta) values %s",
            rows)
    return {"ok": True, "inserted": len(rows)}
