"""
CEX Virtual NPU -- Wire Protocol v1
Shared between client (Windows i5-7500) and server (Qualcomm ARM Linux).

Binary frame layout:
  [0-3]   Magic      : 4 bytes  b"CXNP"
  [4-7]   Type       : uint32 LE  (see MsgType enum)
  [8-11]  Payload len: uint32 LE  (bytes after header)
  [12+]   Payload    : JSON metadata + optional raw tensor blob

Tensor blob layout (appended after JSON metadata, delimited by TENSOR_SEP):
  4 bytes  : number of tensors (uint32 LE)
  per tensor:
    4 bytes  : name length (uint32 LE)
    N bytes  : name (UTF-8)
    4 bytes  : dtype code (uint32 LE)  -- see DType enum
    4 bytes  : ndim (uint32 LE)
    4*ndim   : shape (uint32 LE each)
    4 bytes  : data length (uint32 LE)
    N bytes  : raw float/int data

Ports (from .cex/config/network_config.yaml):
  7474 -- RX : server listens (client sends inference requests)
  7475 -- TX : server pushes results (client receives)
  7476 -- health : discovery / heartbeat
"""

import struct
import json
import numpy as np
from enum import IntEnum
from typing import Dict, List, Optional, Tuple


MAGIC = b"CXNP"
HEADER_SIZE = 12  # magic(4) + type(4) + payload_len(4)

DEFAULT_RX_PORT = 7474
DEFAULT_TX_PORT = 7475
DEFAULT_HEALTH_PORT = 7476

PROTOCOL_VERSION = 1
MAX_PAYLOAD_BYTES = 256 * 1024 * 1024  # 256 MB hard cap


class MsgType(IntEnum):
    CAPABILITIES_REQUEST  = 0x01
    CAPABILITIES_RESPONSE = 0x02
    INFER_REQUEST         = 0x10
    INFER_RESPONSE        = 0x11
    HEARTBEAT             = 0x20
    HEARTBEAT_ACK         = 0x21
    CANCEL_REQUEST        = 0x30
    ERROR                 = 0xFF


class DType(IntEnum):
    FLOAT32 = 1
    FLOAT16 = 2
    INT32   = 3
    INT64   = 4
    UINT8   = 5
    INT8    = 6
    BOOL    = 7


DTYPE_TO_NP = {
    DType.FLOAT32: np.float32,
    DType.FLOAT16: np.float16,
    DType.INT32:   np.int32,
    DType.INT64:   np.int64,
    DType.UINT8:   np.uint8,
    DType.INT8:    np.int8,
    DType.BOOL:    np.bool_,
}

NP_TO_DTYPE = {v: k for k, v in DTYPE_TO_NP.items()}


def encode_header(msg_type: MsgType, payload_len: int) -> bytes:
    return MAGIC + struct.pack("<II", int(msg_type), payload_len)


def decode_header(data: bytes) -> Tuple[MsgType, int]:
    if len(data) < HEADER_SIZE:
        raise ValueError("Header too short: {} bytes".format(len(data)))
    if data[:4] != MAGIC:
        raise ValueError("Bad magic: {}".format(data[:4]))
    msg_type, payload_len = struct.unpack("<II", data[4:12])
    if payload_len > MAX_PAYLOAD_BYTES:
        raise ValueError("Payload too large: {} bytes".format(payload_len))
    return MsgType(msg_type), payload_len


def encode_tensors(tensors: Dict[str, np.ndarray]) -> bytes:
    """Serialize a dict of named numpy arrays to binary blob."""
    buf = struct.pack("<I", len(tensors))
    for name, arr in tensors.items():
        name_bytes = name.encode("utf-8")
        dtype_code = int(NP_TO_DTYPE.get(arr.dtype.type, DType.FLOAT32))
        shape = list(arr.shape)
        raw = arr.astype(DTYPE_TO_NP.get(dtype_code, np.float32)).tobytes()
        buf += struct.pack("<I", len(name_bytes))
        buf += name_bytes
        buf += struct.pack("<I", dtype_code)
        buf += struct.pack("<I", len(shape))
        for s in shape:
            buf += struct.pack("<I", s)
        buf += struct.pack("<I", len(raw))
        buf += raw
    return buf


def decode_tensors(blob: bytes) -> Dict[str, np.ndarray]:
    """Deserialize binary blob back to dict of named numpy arrays."""
    offset = 0
    n_tensors, = struct.unpack_from("<I", blob, offset)
    offset += 4
    result = {}
    for _ in range(n_tensors):
        name_len, = struct.unpack_from("<I", blob, offset);  offset += 4
        name = blob[offset:offset + name_len].decode("utf-8");  offset += name_len
        dtype_code, = struct.unpack_from("<I", blob, offset);  offset += 4
        ndim, = struct.unpack_from("<I", blob, offset);  offset += 4
        shape = []
        for _ in range(ndim):
            s, = struct.unpack_from("<I", blob, offset);  offset += 4
            shape.append(s)
        data_len, = struct.unpack_from("<I", blob, offset);  offset += 4
        raw = blob[offset:offset + data_len];  offset += data_len
        dtype_np = DTYPE_TO_NP.get(DType(dtype_code), np.float32)
        result[name] = np.frombuffer(raw, dtype=dtype_np).reshape(shape)
    return result


def build_infer_request(
    request_id: str,
    model_bytes: bytes,
    input_tensors: Dict[str, np.ndarray],
    output_names: Optional[List[str]] = None,
) -> bytes:
    """Build a complete INFER_REQUEST frame."""
    tensor_blob = encode_tensors(input_tensors)
    meta = {
        "version": PROTOCOL_VERSION,
        "request_id": request_id,
        "model_size": len(model_bytes),
        "output_names": output_names or [],
    }
    meta_bytes = json.dumps(meta).encode("utf-8")
    # payload = meta_len(4) + meta + model_bytes + tensor_blob
    payload = (
        struct.pack("<I", len(meta_bytes))
        + meta_bytes
        + struct.pack("<I", len(model_bytes))
        + model_bytes
        + tensor_blob
    )
    header = encode_header(MsgType.INFER_REQUEST, len(payload))
    return header + payload


def parse_infer_request(
    payload: bytes,
) -> Tuple[dict, bytes, Dict[str, np.ndarray]]:
    """Parse INFER_REQUEST payload. Returns (meta, model_bytes, input_tensors)."""
    offset = 0
    meta_len, = struct.unpack_from("<I", payload, offset);  offset += 4
    meta = json.loads(payload[offset:offset + meta_len]);  offset += meta_len
    model_len, = struct.unpack_from("<I", payload, offset);  offset += 4
    model_bytes = payload[offset:offset + model_len];  offset += model_len
    input_tensors = decode_tensors(payload[offset:])
    return meta, model_bytes, input_tensors


def build_infer_response(
    request_id: str,
    output_tensors: Dict[str, np.ndarray],
    latency_ms: float,
    error: Optional[str] = None,
) -> bytes:
    """Build a complete INFER_RESPONSE frame."""
    meta = {
        "version": PROTOCOL_VERSION,
        "request_id": request_id,
        "latency_ms": latency_ms,
        "error": error,
    }
    meta_bytes = json.dumps(meta).encode("utf-8")
    tensor_blob = encode_tensors(output_tensors) if output_tensors else b""
    payload = (
        struct.pack("<I", len(meta_bytes))
        + meta_bytes
        + tensor_blob
    )
    header = encode_header(MsgType.INFER_RESPONSE, len(payload))
    return header + payload


def parse_infer_response(
    payload: bytes,
) -> Tuple[dict, Dict[str, np.ndarray]]:
    """Parse INFER_RESPONSE payload. Returns (meta, output_tensors)."""
    offset = 0
    meta_len, = struct.unpack_from("<I", payload, offset);  offset += 4
    meta = json.loads(payload[offset:offset + meta_len]);  offset += meta_len
    output_tensors = decode_tensors(payload[offset:]) if offset < len(payload) else {}
    return meta, output_tensors


def build_heartbeat() -> bytes:
    meta = json.dumps({"version": PROTOCOL_VERSION}).encode("utf-8")
    return encode_header(MsgType.HEARTBEAT, len(meta)) + meta


def build_heartbeat_ack(npu_util_pct: float, queue_depth: int) -> bytes:
    meta = json.dumps({
        "version": PROTOCOL_VERSION,
        "npu_util_pct": npu_util_pct,
        "queue_depth": queue_depth,
    }).encode("utf-8")
    return encode_header(MsgType.HEARTBEAT_ACK, len(meta)) + meta


def build_error(request_id: str, message: str, code: int = 500) -> bytes:
    meta = json.dumps({
        "request_id": request_id,
        "error": message,
        "code": code,
    }).encode("utf-8")
    return encode_header(MsgType.ERROR, len(meta)) + meta
