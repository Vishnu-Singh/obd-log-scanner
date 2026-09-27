"""Raw CAN frame ingest API.

Run:  OBD_API_KEY=change-me uvicorn app:app --host 0.0.0.0 --port 8000
"""

import os
import secrets
from contextlib import asynccontextmanager

from fastapi import Depends, FastAPI, Header, HTTPException, Query
from pydantic import BaseModel, Field, field_validator

import storage

API_KEY = os.environ.get("OBD_API_KEY", "change-me")
DB_PATH = os.environ.get("OBD_DB_PATH", "obd_raw.db")


@asynccontextmanager
async def lifespan(_: FastAPI):
    storage.init_db(DB_PATH)
    yield


app = FastAPI(title="OBD raw ingest", lifespan=lifespan)


def require_key(x_api_key: str = Header(default="")) -> None:
    if not secrets.compare_digest(x_api_key, API_KEY):
        raise HTTPException(status_code=401, detail="invalid API key")


class FrameIn(BaseModel):
    t: int = Field(ge=0, description="device uptime in microseconds")
    bus: int = Field(0, ge=0, le=1, description="0 = HS-CAN, 1 = MS-CAN")
    id: int = Field(ge=0, le=0x1FFFFFFF)
    ext: int = Field(0, ge=0, le=1)
    dir: int = Field(0, ge=0, le=1)
    data: str = Field(max_length=16, description="payload as hex, up to 8 bytes")

    @field_validator("data")
    @classmethod
    def must_be_hex(cls, v: str) -> str:
        if len(v) % 2:
            raise ValueError("hex string must have even length")
        bytes.fromhex(v)
        return v.upper()


class BatchIn(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    boot_id: str = Field(min_length=1, max_length=32)
    sent_us: int = Field(ge=0)
    dropped: int = Field(0, ge=0)
    frames: list[FrameIn] = Field(max_length=2000)


@app.get("/health")
def health():
    return {"ok": True}


@app.post("/api/v1/frames", dependencies=[Depends(require_key)])
def ingest(batch: BatchIn):
    batch_id, stored = storage.store_batch(
        DB_PATH,
        device_id=batch.device_id,
        boot_id=batch.boot_id,
        sent_us=batch.sent_us,
        dropped=batch.dropped,
        frames=[f.model_dump() for f in batch.frames],
    )
    return {"ok": True, "batch_id": batch_id, "stored": stored}


@app.get("/api/v1/frames", dependencies=[Depends(require_key)])
def list_frames(
    since: float = 0.0,
    can_id: str | None = Query(None, description="e.g. 0x7E8 or 2024"),
    bus: str | None = Query(None, description="hs or ms"),
    limit: int = Query(500, ge=1, le=5000),
):
    cid = None
    if can_id is not None:
        try:
            cid = int(can_id, 0)
        except ValueError:
            raise HTTPException(status_code=400, detail="can_id must be an integer, e.g. 0x7E8")
    bus_id = None
    if bus is not None:
        if bus not in ("hs", "ms"):
            raise HTTPException(status_code=400, detail="bus must be 'hs' or 'ms'")
        bus_id = 0 if bus == "hs" else 1
    return storage.query_frames(DB_PATH, since=since, can_id=cid, bus=bus_id, limit=limit)


@app.get("/api/v1/stats", dependencies=[Depends(require_key)])
def get_stats():
    return storage.stats(DB_PATH)