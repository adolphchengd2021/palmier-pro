from __future__ import annotations

import argparse
import ctypes
import http.client
import json
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any


PROTOCOL_VERSION = "2025-06-18"


class TestFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def normalized(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def parse_mcp_body(content_type: str, body: bytes) -> dict[str, Any]:
    text = body.decode("utf-8")
    if "text/event-stream" in content_type:
        data = "\n".join(
            line[5:].lstrip()
            for line in text.splitlines()
            if line.startswith("data:")
        )
        return json.loads(data)
    return json.loads(text)


class Client:
    def __init__(self, port: int) -> None:
        self.port = port
        self.session_id: str | None = None
        self.next_id = 1

    def raw(
        self,
        method: str,
        body: bytes = b"",
        *,
        origin: str | None = None,
        content_type: str = "application/json",
        accept: str = "application/json, text/event-stream",
        include_session: bool = True,
        include_protocol: bool = True,
    ) -> tuple[int, dict[str, str], bytes]:
        headers = {
            "Accept": accept,
            "Content-Type": content_type,
            "Origin": origin or f"http://127.0.0.1:{self.port}",
        }
        if include_session and self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if include_protocol and self.session_id:
            headers["MCP-Protocol-Version"] = PROTOCOL_VERSION
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request(method, "/mcp", body=body, headers=headers)
            response = connection.getresponse()
            response_body = response.read()
            response_headers = {key.lower(): value for key, value in response.getheaders()}
            return response.status, response_headers, response_body
        finally:
            connection.close()

    def initialize(self) -> None:
        request_id = self.next_id
        self.next_id += 1
        payload = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "initialize",
            "params": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "palmier-windows-e2e", "version": "1"},
            },
        }
        status, headers, body = self.raw(
            "POST",
            normalized(payload).encode("utf-8"),
            include_session=False,
            include_protocol=False,
        )
        require(status == 200, f"initialize status: {status} {body!r}")
        response = parse_mcp_body(headers.get("content-type", ""), body)
        require(response.get("id") == request_id, "initialize response id")
        result = response.get("result", {})
        require(result.get("protocolVersion") == PROTOCOL_VERSION, "initialize protocol")
        require(result.get("serverInfo", {}).get("name") == "palmier-pro-windows", "server name")
        self.session_id = headers.get("mcp-session-id")
        require(bool(self.session_id), "initialize session header")
        notification = {
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
            "params": {},
        }
        status, _, body = self.raw("POST", normalized(notification).encode("utf-8"))
        require(status == 202 and not body, "initialized notification")

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        payload: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            payload["params"] = params
        status, headers, body = self.raw("POST", normalized(payload).encode("utf-8"))
        require(status == 200, f"{method} status: {status} {body!r}")
        response = parse_mcp_body(headers.get("content-type", ""), body)
        require(response.get("id") == request_id, f"{method} response id")
        require("error" not in response, f"{method} JSON-RPC error: {response}")
        return response["result"]

    def tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def close(self) -> None:
        if not self.session_id:
            return
        status, _, body = self.raw("DELETE")
        require(status == 204 and not body, "session DELETE")
        self.session_id = None


def tool_payload(result: dict[str, Any]) -> dict[str, Any]:
    require(result.get("isError") is not True, f"unexpected tool failure: {result}")
    content = result.get("content", [])
    require(len(content) == 1 and content[0].get("type") == "text", "tool text content")
    payload = json.loads(content[0]["text"])
    require(content[0]["text"] == normalized(payload), "tool payload must be canonical JSON")
    return payload


def tool_error(result: dict[str, Any], expected_code: str) -> dict[str, Any]:
    require(result.get("isError") is True, f"expected tool failure: {result}")
    content = result.get("content", [])
    require(len(content) == 1 and content[0].get("type") == "text", "tool error text content")
    payload = json.loads(content[0]["text"])
    require(content[0]["text"] == normalized(payload), "tool error must be canonical JSON")
    require(payload.get("code") == expected_code, f"tool error code: {payload}")
    require(isinstance(payload.get("message"), str) and payload["message"], "tool error message")
    return payload


def require_receipt(
    receipt: dict[str, Any],
    contract: dict[str, Any],
    revision_before: int,
    revision_after: int,
) -> str:
    expected_fields = set(contract["toolResult"]["mutationReceiptFields"])
    require(set(receipt) == expected_fields, f"mutation receipt fields: {sorted(receipt)}")
    require(receipt.get("changed") is True, "mutation receipt changed")
    require(receipt.get("revisionBefore") == revision_before, "receipt revisionBefore")
    require(receipt.get("revisionAfter") == revision_after, "receipt revisionAfter")
    action_id = receipt.get("actionId")
    require(isinstance(action_id, str) and action_id, "receipt actionId")
    return action_id


def require_noop_receipt(
    receipt: dict[str, Any],
    contract: dict[str, Any],
    revision: int,
) -> None:
    expected_fields = set(contract["toolResult"]["mutationReceiptFields"])
    require(set(receipt) == expected_fields, f"no-op receipt fields: {sorted(receipt)}")
    require(receipt.get("changed") is False, "no-op receipt changed")
    require(receipt.get("revisionBefore") == revision, "no-op revisionBefore")
    require(receipt.get("revisionAfter") == revision, "no-op revisionAfter")
    require(receipt.get("actionId") == "", "no-op actionId")


def timeline_clip(timeline: dict[str, Any], clip_id: str) -> dict[str, Any] | None:
    for track in timeline.get("tracks", []):
        for clip in track.get("clips", []):
            if clip.get("id") == clip_id:
                return clip
    return None


def wait_ready(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TestFailure(f"server exited before READY with code {process.returncode}")
        try:
            line = lines.get(timeout=0.2)
        except queue.Empty:
            continue
        if line.startswith("PALMIER_WINDOWS_MCP_READY "):
            return
    raise TestFailure("server did not report READY within 15 seconds")


def require_port_released(port: int) -> None:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        probe.bind(("127.0.0.1", port))
    finally:
        probe.close()


def require_loopback_listener(process_id: int, port: int) -> None:
    require(sys.platform == "win32", "listener inspection requires Windows")

    class TcpRowOwnerPid(ctypes.Structure):
        _fields_ = [
            ("state", ctypes.c_ulong),
            ("local_address", ctypes.c_ulong),
            ("local_port", ctypes.c_ulong),
            ("remote_address", ctypes.c_ulong),
            ("remote_port", ctypes.c_ulong),
            ("owning_pid", ctypes.c_ulong),
        ]

    get_table = ctypes.WinDLL("iphlpapi").GetExtendedTcpTable
    get_table.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.c_bool,
        ctypes.c_ulong,
        ctypes.c_int,
        ctypes.c_ulong,
    ]
    get_table.restype = ctypes.c_ulong
    size = ctypes.c_ulong(0)
    result = get_table(None, ctypes.byref(size), False, socket.AF_INET, 3, 0)
    require(result in (0, 122), f"GetExtendedTcpTable size result: {result}")
    buffer = ctypes.create_string_buffer(size.value)
    result = get_table(buffer, ctypes.byref(size), False, socket.AF_INET, 3, 0)
    require(result == 0, f"GetExtendedTcpTable result: {result}")
    count = ctypes.c_ulong.from_buffer(buffer).value
    rows = (TcpRowOwnerPid * count).from_address(
        ctypes.addressof(buffer) + ctypes.sizeof(ctypes.c_ulong)
    )
    addresses = {
        socket.inet_ntoa(struct.pack("<I", row.local_address))
        for row in rows
        if row.state == 2
        and row.owning_pid == process_id
        and socket.ntohs(row.local_port & 0xFFFF) == port
    }
    require(addresses == {"127.0.0.1"}, f"listener addresses: {sorted(addresses)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()

    contract = json.loads(args.contract.read_text(encoding="utf-8"))
    clients: list[Client] = []
    captured_stdout: list[str] = []
    with tempfile.TemporaryDirectory(prefix="palmier-mcp-e2e-") as temporary:
        project = Path(temporary) / "isolated.palmier"
        shutil.copytree(args.fixture, project)
        process = subprocess.Popen(
            [
                str(args.server),
                "--project",
                str(project),
                "--port",
                str(args.port),
                "--exit-after-last-session",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        require(process.stdout is not None and process.stderr is not None, "server pipes")
        output_queue: queue.Queue[str] = queue.Queue()

        def collect_stdout() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                captured_stdout.append(line.rstrip("\r\n"))
                output_queue.put(line.rstrip("\r\n"))

        output_thread = threading.Thread(target=collect_stdout, daemon=True)
        output_thread.start()
        partial: socket.socket | None = None
        try:
            wait_ready(process, output_queue)
            require_loopback_listener(process.pid, args.port)

            partial = socket.create_connection(("127.0.0.1", args.port), timeout=2)
            partial.sendall(b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n")

            first = Client(args.port)
            clients.append(first)
            first.initialize()
            partial.close()
            partial = None

            hostile = Client(args.port)
            bad_initialize = normalized({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": PROTOCOL_VERSION},
            }).encode("utf-8")
            status, _, _ = hostile.raw(
                "POST",
                bad_initialize,
                origin="https://attacker.invalid",
                include_session=False,
                include_protocol=False,
            )
            require(status == 403, "hostile Origin must be rejected")
            status, _, _ = hostile.raw(
                "POST",
                bad_initialize,
                content_type="text/plain",
                include_session=False,
                include_protocol=False,
            )
            require(status == 415, "wrong Content-Type must be rejected")
            status, _, _ = hostile.raw(
                "POST",
                bad_initialize,
                content_type="text/plain; note=application/json",
                include_session=False,
                include_protocol=False,
            )
            require(status == 415, "embedded JSON Content-Type token must be rejected")
            status, _, _ = hostile.raw(
                "POST",
                bad_initialize,
                accept="application/json-evil",
                include_session=False,
                include_protocol=False,
            )
            require(status == 406, "invalid Accept media type must be rejected")

            listed = first.request("tools/list")
            require(listed.get("tools") == contract["tools"], "technical MVP discovery schema drift")

            baseline = tool_payload(first.tool("get_timeline", {}))
            baseline_clip = timeline_clip(baseline, "clip-main-1")
            require(baseline_clip is not None, "baseline clip")
            require(baseline_clip["frames"] == [0, 150], "baseline frame range")
            require(baseline_clip["mediaRef"] == "media-main-1", "baseline mediaRef")
            tool_error(
                first.tool("get_timeline", {"captionDetail": True}),
                "unsupportedTimelineQuery",
            )

            receipt = tool_payload(first.tool(
                "split_clips",
                {"splits": [{"clipId": "clip-main-1", "atFrame": 60}]},
            ))
            first_action_id = require_receipt(receipt, contract, 0, 1)
            receipt_clips = receipt.get("clips", [])
            require(len(receipt_clips) == 2, "split receipt clip count")
            right_id = next(
                clip["id"] for clip in receipt_clips if clip["id"] != "clip-main-1"
            )

            second = Client(args.port)
            clients.append(second)
            second.initialize()
            split_readback = tool_payload(second.tool("get_timeline", {}))
            left = timeline_clip(split_readback, "clip-main-1")
            right = timeline_clip(split_readback, right_id)
            require(left is not None and right is not None, "cross-session split readback")
            require(left["frames"] == [0, 60] and left["trimEndFrame"] == 90, "left split state")
            require(right["frames"] == [60, 150] and right["trimStartFrame"] == 60, "right split state")

            second_receipt = tool_payload(second.tool(
                "split_clips",
                {"splits": [{"clipId": right_id, "atFrame": 100}]},
            ))
            second_action_id = require_receipt(second_receipt, contract, 1, 2)
            require(second_action_id != first_action_id, "split actions need distinct IDs")
            second_split_readback = tool_payload(first.tool("get_timeline", {}))
            require(
                sum(len(track.get("clips", [])) for track in second_split_readback["tracks"]) == 3,
                "generated right ID must remain editable",
            )
            invalid = second.tool(
                "split_clips",
                {"splits": [{"clipId": "clip-main-1", "atFrame": -1}]},
            )
            tool_error(invalid, "invalidSplitFrame")
            tool_error(second.tool("split_clips", {"trackIndex": 0}), "invalidSplitMode")
            tool_error(second.tool("split_clips", {"frames": [30]}), "invalidSplitMode")
            unchanged = tool_payload(second.tool("get_timeline", {}))
            require(
                normalized(unchanged) == normalized(second_split_readback),
                "rejected requests must not mutate",
            )

            first_undo = tool_payload(second.tool("undo", {}))
            require(require_receipt(first_undo, contract, 2, 3) == second_action_id, "undo action identity")
            first_restored = tool_payload(first.tool("get_timeline", {}))
            require(normalized(first_restored) == normalized(split_readback), "undo second split")
            second_undo = tool_payload(first.tool("undo", {}))
            require(require_receipt(second_undo, contract, 3, 4) == first_action_id, "shared undo identity")
            restored = tool_payload(second.tool("get_timeline", {}))
            require(normalized(restored) == normalized(baseline), "cross-session undo exact restore")

            tool_error(first.tool("move_clips", {"moves": []}), "invalidMoves")
            tool_error(first.tool(
                "move_clips",
                {"moves": [{"clipId": "clip-main-1"}]},
            ), "invalidMoveDestination")
            tool_error(first.tool(
                "move_clips",
                {"moves": [{"clipId": "clip-main-1", "toFrame": -1}]},
            ), "invalidMoveFrame")
            no_op = tool_payload(first.tool(
                "move_clips",
                {"moves": [{"clipId": "clip-main-1", "toFrame": 0}]},
            ))
            require_noop_receipt(no_op, contract, 4)
            require(
                normalized(tool_payload(second.tool("get_timeline", {}))) == normalized(baseline),
                "move no-op changed timeline",
            )
            move_receipt = tool_payload(first.tool(
                "move_clips",
                {"moves": [{"clipId": "clip-main-1", "toFrame": 200}]},
            ))
            move_action_id = require_receipt(move_receipt, contract, 4, 5)
            moved = tool_payload(second.tool("get_timeline", {}))
            moved_clip = timeline_clip(moved, "clip-main-1")
            require(moved_clip is not None and moved_clip["frames"] == [200, 350], "move readback")
            move_undo = tool_payload(second.tool("undo", {}))
            require(require_receipt(move_undo, contract, 5, 6) == move_action_id, "move undo identity")
            require(
                normalized(tool_payload(first.tool("get_timeline", {}))) == normalized(baseline),
                "move undo exact restore",
            )
            tool_error(first.tool("remove_clips", {"clipIds": []}), "invalidClipIds")
            tool_error(first.tool(
                "remove_clips",
                {"clipIds": ["clip-main-1", "missing"]},
            ), "clipNotFound")
            remove_receipt = tool_payload(first.tool(
                "remove_clips",
                {"clipIds": ["clip-main-1"]},
            ))
            remove_action_id = require_receipt(remove_receipt, contract, 6, 7)
            require(
                remove_receipt["removedClipIds"] == ["clip-main-1"],
                "remove receipt stable IDs",
            )
            removed = tool_payload(second.tool("get_timeline", {}))
            require(timeline_clip(removed, "clip-main-1") is None, "remove readback")
            remove_undo = tool_payload(second.tool("undo", {}))
            require(
                require_receipt(remove_undo, contract, 7, 8) == remove_action_id,
                "remove undo identity",
            )
            require(
                normalized(tool_payload(first.tool("get_timeline", {}))) == normalized(baseline),
                "remove undo exact restore",
            )
            tool_error(first.tool("undo", {}), "nothingToUndo")

            unknown = first.tool("set_clip_properties", {"clipIds": ["clip-main-1"]})
            tool_error(unknown, "toolNotImplemented")

            overflow_clients: list[Client] = []
            for _ in range(30):
                overflow = Client(args.port)
                clients.append(overflow)
                overflow_clients.append(overflow)
                overflow.initialize()
            invalid_capacity_initialize = normalized({
                "jsonrpc": "2.0",
                "id": 1000,
                "method": "initialize",
                "params": {"protocolVersion": "invalid"},
            }).encode("utf-8")
            status, headers, body = hostile.raw(
                "POST",
                invalid_capacity_initialize,
                include_session=False,
                include_protocol=False,
            )
            require(status == 200, "invalid initialize transport status")
            invalid_response = parse_mcp_body(headers.get("content-type", ""), body)
            require(invalid_response.get("error", {}).get("code") == -32602, "invalid protocol error")
            require("mcp-session-id" not in headers, "invalid initialize must not create a session")
            first.request("tools/list")
            for _ in range(3):
                overflow = Client(args.port)
                clients.append(overflow)
                overflow_clients.append(overflow)
                overflow.initialize()
            stale_request = normalized({
                "jsonrpc": "2.0",
                "id": 999,
                "method": "tools/list",
            }).encode("utf-8")
            status, _, _ = second.raw("POST", stale_request)
            require(status == 404, "least-recent session must be evicted at capacity")
            second.session_id = None
            overflow_clients[0].session_id = None
            overflow_clients[1].session_id = None
            for overflow in overflow_clients[2:]:
                overflow.close()
            first.close()
            process.wait(timeout=10)
            require(process.returncode == 0, f"server exit code {process.returncode}")
            output_thread.join(timeout=2)
            stderr = process.stderr.read()
            require(not stderr, f"server stderr: {stderr}")
            require_port_released(args.port)
        except Exception as error:
            if partial is not None:
                partial.close()
            for client in reversed(clients):
                try:
                    client.close()
                except Exception:
                    pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            stderr = process.stderr.read() if process.stderr else ""
            diagnostics = "\n".join(captured_stdout)
            if stderr:
                diagnostics += "\nSTDERR\n" + stderr
            raise TestFailure(f"{error}\nSERVER OUTPUT\n{diagnostics}") from error

    print("PALMIER_MCP_HTTP_E2E_OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"PALMIER_MCP_HTTP_E2E_FAILED {error}")
        raise SystemExit(1)
