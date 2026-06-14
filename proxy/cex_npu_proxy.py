"""
CEX Virtual NPU -- Network Proxy (Windows i5-7500 side)
Receives inference requests locally (from EP or named pipe) and forwards
to the Qualcomm ARM server via TCP port 7474.

This runs as a Windows service (see cex_npu_service.py).
It bridges the local ONNX Runtime Execution Provider (cex_npu_ep.dll)
to the remote Qualcomm NPU over the LAN.

Named pipe: \\.\pipe\CexNpuProxy  (local IPC from EP DLL -> this proxy)
TCP upstream: <server_host>:7474  (to ARM Linux server)

Usage:
  python cex_npu_proxy.py --server 192.168.1.100 --rx-port 7474
  python cex_npu_proxy.py --server 192.168.1.100 --scan   # auto-discover
"""

import os
import sys
import time
import uuid
import json
import socket
import struct
import logging
import argparse
import threading
import queue
from typing import Dict, Optional, Tuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "shared"))
from cex_npu_protocol import (
    HEADER_SIZE,
    MsgType,
    decode_header,
    build_infer_request,
    parse_infer_response,
    build_heartbeat,
    DEFAULT_RX_PORT,
    DEFAULT_TX_PORT,
    DEFAULT_HEALTH_PORT,
)
import numpy as np

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("cex_npu_proxy")

_PIPE_NAME = r"\\.\pipe\CexNpuProxy"
_CONNECT_TIMEOUT = 5.0
_REQUEST_TIMEOUT = float(os.environ.get("CEX_NPU_TIMEOUT", "30.0"))
_HEARTBEAT_INTERVAL = 5.0

# Pending requests: request_id -> result queue
_pending: Dict[str, queue.Queue] = {}
_pending_lock = threading.Lock()

# TCP connection to upstream ARM server
_upstream_sock: Optional[socket.socket] = None
_upstream_lock = threading.Lock()
_server_host: str = ""
_server_port: int = DEFAULT_RX_PORT


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Upstream closed")
        buf.extend(chunk)
    return bytes(buf)


def _send_all(sock: socket.socket, data: bytes) -> None:
    total = 0
    while total < len(data):
        sent = sock.send(data[total:])
        if not sent:
            raise ConnectionError("Upstream send failed")
        total += sent


def _connect_upstream() -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(_CONNECT_TIMEOUT)
    sock.connect((_server_host, _server_port))
    sock.settimeout(None)
    logger.info("Connected to upstream %s:%d", _server_host, _server_port)
    return sock


def _get_upstream() -> socket.socket:
    global _upstream_sock
    with _upstream_lock:
        if _upstream_sock is None:
            _upstream_sock = _connect_upstream()
        return _upstream_sock


def _reset_upstream():
    global _upstream_sock
    with _upstream_lock:
        if _upstream_sock:
            try:
                _upstream_sock.close()
            except Exception:
                pass
            _upstream_sock = None


def _upstream_reader_thread():
    """Reads responses from the ARM server and dispatches to waiting callers."""
    while True:
        try:
            sock = _get_upstream()
            header = _recv_exact(sock, HEADER_SIZE)
            msg_type, payload_len = decode_header(header)
            payload = _recv_exact(sock, payload_len) if payload_len > 0 else b""

            if msg_type == MsgType.INFER_RESPONSE:
                meta, output_tensors = parse_infer_response(payload)
                request_id = meta.get("request_id", "")
                with _pending_lock:
                    q = _pending.pop(request_id, None)
                if q:
                    q.put(("ok", output_tensors, meta.get("latency_ms", 0.0)))
                else:
                    logger.warning("Orphan response for request_id=%s", request_id)

            elif msg_type == MsgType.HEARTBEAT_ACK:
                info = json.loads(payload)
                logger.debug(
                    "Heartbeat ACK: npu_util=%.1f%% queue=%d",
                    info.get("npu_util_pct", 0),
                    info.get("queue_depth", 0),
                )
                # Update perf counter for Task Manager (via shared memory or pipe)
                _update_perf_counter(info.get("npu_util_pct", 0.0))

            elif msg_type == MsgType.ERROR:
                info = json.loads(payload)
                request_id = info.get("request_id", "")
                with _pending_lock:
                    q = _pending.pop(request_id, None)
                if q:
                    q.put(("error", info.get("error", "unknown"), 0.0))

        except ConnectionError as e:
            logger.warning("Upstream connection lost: %s -- reconnecting...", e)
            _reset_upstream()
            _fail_all_pending("Upstream connection lost: {}".format(e))
            time.sleep(2.0)
        except Exception as e:
            logger.exception("Upstream reader error: %s", e)
            _reset_upstream()
            _fail_all_pending(str(e))
            time.sleep(2.0)


def _fail_all_pending(reason: str):
    with _pending_lock:
        for rid, q in _pending.items():
            q.put(("error", reason, 0.0))
        _pending.clear()


def _heartbeat_thread():
    """Sends periodic heartbeats to keep connection alive and get NPU stats."""
    while True:
        time.sleep(_HEARTBEAT_INTERVAL)
        try:
            sock = _get_upstream()
            _send_all(sock, build_heartbeat())
        except Exception as e:
            logger.debug("Heartbeat failed: %s", e)


_perf_counter_value: float = 0.0
_perf_counter_lock = threading.Lock()


def _update_perf_counter(value: float):
    """Update the shared memory / named event for Windows Task Manager NPU counter."""
    global _perf_counter_value
    with _perf_counter_lock:
        _perf_counter_value = value
    # On Windows: write to shared memory mapped by cex_virt_npu.sys
    # The driver reads from \\.\CexNpuSharedMem to get utilization for Task Manager.
    # Implementation: see driver/wddm/cex_virt_npu.c _CexUpdateUtilization()
    try:
        import mmap
        # Shared memory name matches driver's CreateFileMapping name
        # Format: 4-byte float (utilization 0.0-100.0)
        shm = mmap.mmap(-1, 4, tagname="CexNpuUtil")
        shm.seek(0)
        shm.write(struct.pack("<f", value))
        shm.close()
    except Exception:
        pass  # driver shared memory may not be mapped yet


def infer(
    model_bytes: bytes,
    input_tensors: Dict[str, np.ndarray],
    output_names=None,
    timeout: float = _REQUEST_TIMEOUT,
) -> Tuple[Dict[str, np.ndarray], float]:
    """
    Submit inference request to the Qualcomm NPU server.
    Blocks until result is returned or timeout.
    Returns (output_tensors, latency_ms).
    Called by cex_npu_ep.dll via named pipe or directly via Python API.
    """
    request_id = str(uuid.uuid4())
    result_queue: queue.Queue = queue.Queue(maxsize=1)

    frame = build_infer_request(request_id, model_bytes, input_tensors, output_names)

    with _pending_lock:
        _pending[request_id] = result_queue

    try:
        sock = _get_upstream()
        _send_all(sock, frame)
    except Exception as e:
        with _pending_lock:
            _pending.pop(request_id, None)
        raise RuntimeError("Failed to send request: {}".format(e)) from e

    try:
        status, result, latency = result_queue.get(timeout=timeout)
    except queue.Empty:
        with _pending_lock:
            _pending.pop(request_id, None)
        raise TimeoutError("Inference timeout after {:.0f}s".format(timeout))

    if status == "error":
        raise RuntimeError("Server error: {}".format(result))

    return result, latency


def _pipe_server_thread():
    """
    Named pipe server -- accepts connections from cex_npu_ep.dll.
    Protocol over pipe: same binary CXNP frames as TCP.
    """
    try:
        import win32pipe
        import win32file
        import pywintypes
    except ImportError:
        logger.warning(
            "pywin32 not available -- named pipe disabled. "
            "Direct Python API still works."
        )
        return

    logger.info("Named pipe server: %s", _PIPE_NAME)
    while True:
        try:
            pipe = win32pipe.CreateNamedPipe(
                _PIPE_NAME,
                win32pipe.PIPE_ACCESS_DUPLEX,
                win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
                win32pipe.PIPE_UNLIMITED_INSTANCES,
                65536, 65536, 0, None,
            )
            win32pipe.ConnectNamedPipe(pipe, None)
            t = threading.Thread(
                target=_handle_pipe_client,
                args=(pipe,),
                daemon=True,
            )
            t.start()
        except Exception as e:
            logger.exception("Pipe server error: %s", e)
            time.sleep(1.0)


def _handle_pipe_client(pipe):
    """Handle one named pipe client connection (from EP DLL)."""
    import win32file
    try:
        while True:
            # Read header
            _, header_bytes = win32file.ReadFile(pipe, HEADER_SIZE)
            msg_type, payload_len = decode_header(bytes(header_bytes))
            payload = b""
            if payload_len > 0:
                _, payload_bytes = win32file.ReadFile(pipe, payload_len)
                payload = bytes(payload_bytes)

            if msg_type == MsgType.INFER_REQUEST:
                from cex_npu_protocol import parse_infer_request, build_infer_response, build_error
                meta, model_bytes, input_tensors = parse_infer_request(payload)
                request_id = meta.get("request_id", str(uuid.uuid4()))
                output_names = meta.get("output_names") or None
                try:
                    outputs, latency = infer(model_bytes, input_tensors, output_names)
                    response = build_infer_response(request_id, outputs, latency)
                except Exception as e:
                    response = build_error(request_id, str(e))
                win32file.WriteFile(pipe, response)
    except Exception as e:
        logger.debug("Pipe client closed: %s", e)


def discover_server(subnet: str = None, port: int = DEFAULT_HEALTH_PORT) -> Optional[str]:
    """
    Scan LAN for CEX NPU server via port 7476 /health endpoint.
    Returns first responding host or None.
    """
    import ipaddress
    import urllib.request

    if not subnet:
        # Guess local subnet from default gateway
        subnet = "192.168.1.0/24"

    logger.info("Scanning %s for CEX NPU server on port %d...", subnet, port)
    network = ipaddress.IPv4Network(subnet, strict=False)
    found = None
    found_lock = threading.Lock()
    sem = threading.Semaphore(64)

    def _probe(host):
        nonlocal found
        with sem:
            if found:
                return
            try:
                url = "http://{}:{}/health".format(host, port)
                with urllib.request.urlopen(url, timeout=0.5) as resp:
                    data = json.loads(resp.read())
                    if data.get("npu_available"):
                        with found_lock:
                            if not found:
                                found = str(host)
                                logger.info("Found NPU server: %s (%s)", host, data)
            except Exception:
                pass

    threads = [threading.Thread(target=_probe, args=(str(h),), daemon=True)
               for h in network.hosts()]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=2.0)

    return found


def main():
    p = argparse.ArgumentParser(description="CEX Virtual NPU Proxy (Windows client)")
    p.add_argument("--server",      type=str, default=os.environ.get("CEX_NPU_SERVER", ""))
    p.add_argument("--rx-port",     type=int, default=DEFAULT_RX_PORT)
    p.add_argument("--subnet",      type=str, default=None)
    p.add_argument("--scan",        action="store_true", help="Auto-discover NPU server on LAN")
    args = p.parse_args()

    global _server_host, _server_port
    _server_port = args.rx_port

    if args.scan or not args.server:
        found = discover_server(args.subnet)
        if not found:
            logger.error("No CEX NPU server found on LAN. Start cex_npu_server.py on ARM host.")
            raise SystemExit(1)
        _server_host = found
    else:
        _server_host = args.server

    logger.info("Proxy -> upstream %s:%d", _server_host, _server_port)

    # Start background threads
    reader = threading.Thread(target=_upstream_reader_thread, daemon=True, name="upstream-reader")
    hb = threading.Thread(target=_heartbeat_thread, daemon=True, name="heartbeat")
    pipe = threading.Thread(target=_pipe_server_thread, daemon=True, name="pipe-server")
    reader.start()
    hb.start()
    pipe.start()

    logger.info("CEX NPU Proxy running. Pipe: %s", _PIPE_NAME)

    try:
        while True:
            time.sleep(60)
    except KeyboardInterrupt:
        logger.info("Proxy stopped.")


if __name__ == "__main__":
    main()
