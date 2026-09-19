#!/usr/bin/env python3
"""Process-level coding turn through the same Unix frontend used by CodexUI.

Default: real Codex + real AISuite HTTP transport + mock native Anthropic server.
--live: native Claude instead (explicit opt-in; incurs API charges).
Never emits headers, credentials, or full prompts. All edits are in a temp project.
"""
import argparse
import errno
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


class NativeFixture(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), NativeHandler)
        self.requests = []
        self.failures = []


class NativeHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            assert self.path == "/v1/messages", self.path
            assert self.headers.get("x-api-key") == "aisuite-test-placeholder"
            assert self.headers.get("anthropic-version") == "2023-06-01"
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert body["stream"] and body["thinking"] == {"type": "disabled"}
            phase = len(self.server.requests)
            self.server.requests.append(body)
            tools = body["tools"]
            results = [block for msg in body["messages"] for block in msg["content"] if block["type"] == "tool_result"]
            if phase:
                assert results and results[-1]["tool_use_id"] == f"call_fixture_{phase - 1}", "lost tool result ID"
                assert not results[-1].get("is_error"), "tool reported failure"
            if phase in (0, 2):
                tool = next(t for t in tools if "cmd" in t["input_schema"].get("properties", {}))
                command = "cat calculator.py" if phase == 0 else "python3 -m unittest -q"
                value = {"cmd": command, "yield_time_ms": 1000, "max_output_tokens": 1000}
                block = {"type": "tool_use", "id": f"call_fixture_{phase}", "name": tool["name"], "input": {}}
                delta = {"type": "input_json_delta", "partial_json": json.dumps(value)}
            elif phase == 1:
                assert "return a - b" in results[-1]["content"], "read tool did not return fixture source"
                tool = next(t for t in tools if "input" in t["input_schema"].get("properties", {}) and "patch" in t["description"].lower())
                patch = "*** Begin Patch\n*** Update File: calculator.py\n@@\n-    return a - b\n+    return a + b\n*** End Patch"
                block = {"type": "tool_use", "id": f"call_fixture_{phase}", "name": tool["name"], "input": {}}
                delta = {"type": "input_json_delta", "partial_json": json.dumps({"input": patch})}
            elif phase == 3:
                assert "OK" in results[-1]["content"], "test tool did not return success"
                block = {"type": "text", "text": ""}
                delta = {"type": "text_delta", "text": "Fixed addition in calculator.py and ran the passing unit test."}
            else:
                raise AssertionError("unexpected additional inference/retry")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            def event(value):
                payload = ("event: " + value["type"] + "\r\ndata: " + json.dumps(value) + "\r\n\r\n").encode()
                # Exercise real chunk decoding and fragmented SSE, not only codecs.
                for i in range(0, len(payload), 13):
                    chunk = payload[i:i + 13]
                    self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                    self.wfile.flush()
            event({"type": "message_start", "message": {"id": f"msg_fixture_{phase}", "type": "message", "role": "assistant", "model": body["model"], "content": [], "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 100, "output_tokens": 1}}})
            event({"type": "content_block_start", "index": 0, "content_block": block})
            event({"type": "content_block_delta", "index": 0, "delta": delta})
            event({"type": "content_block_stop", "index": 0})
            event({"type": "message_delta", "delta": {"stop_reason": "end_turn" if phase == 3 else "tool_use", "stop_sequence": None}, "usage": {"output_tokens": 20}})
            event({"type": "message_stop"})
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except Exception as error:
            self.server.failures.append(str(error))
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", required=True)
    parser.add_argument("--codex", default=shutil.which("codex"))
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--model", default=os.getenv("AISUITE_ANTHROPIC_TEST_MODEL", ""))
    args = parser.parse_args()
    if not args.codex:
        print("SKIP: codex executable unavailable")
        return 77
    if args.live and (not os.getenv("ANTHROPIC_API_KEY") or not args.model):
        print("SKIP: live test requires ANTHROPIC_API_KEY and AISUITE_ANTHROPIC_TEST_MODEL (or --model)")
        return 77
    version = subprocess.check_output([args.codex, "--version"], text=True).strip()
    if version != "codex-cli 0.154.0":
        raise RuntimeError(f"compatibility acceptance requires codex-cli 0.154.0, found {version}")
    # Capability preflight: skip only environmental EPERM/EACCES, not product failures.
    try:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
        with socket.socket(socket.AF_UNIX):
            pass
    except OSError as error:
        if error.errno in (errno.EPERM, errno.EACCES):
            print("SKIP: sandbox denies required socket capability")
            return 77
        raise
    mock = None if args.live else NativeFixture()
    if mock:
        threading.Thread(target=mock.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="aisuite-turn-", dir="/tmp") as root:
            root = Path(root)
            home, runtime, project = (root / name for name in ("home", "runtime", "project"))
            for path in (home, runtime, project):
                path.mkdir(mode=0o700)
            (project / "calculator.py").write_text("def add(a, b):\n    return a - b\n")
            test_source = "import unittest\nfrom calculator import add\nclass AdditionTest(unittest.TestCase):\n    def test_add(self):\n        self.assertEqual(add(2, 3), 5)\n"
            (project / "test_calculator.py").write_text(test_source)
            env = os.environ.copy()
            env["XDG_RUNTIME_DIR"] = str(runtime)
            env["XDG_CONFIG_HOME"] = str(root / "config")
            model = args.model if args.live else "aisuite-fixture-model"
            cmd = [args.bridge, "codex", "--agent", "codex", "--model-provider", "anthropic", "--model", model,
                   "--codex-home", str(home), "--app-server-executable", args.codex]
            if mock:
                env["ANTHROPIC_API_KEY"] = "aisuite-test-placeholder"
                cmd += ["--anthropic-base-url", f"http://127.0.0.1:{mock.server_port}"]
            with open(root / "bridge.log", "w+") as log:
                process = subprocess.Popen(cmd, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + (300 if args.live else 90)
                    client = socket.socket(socket.AF_UNIX)
                    client.settimeout(1)
                    while True:
                        try:
                            client.connect(str(runtime / "codex-bridge.sock"))
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if process.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError("bridge exited or failed to open frontend")
                            time.sleep(0.05)
                    data = bytearray()
                    serial = 0
                    items, text, tool_completed = [], [], []
                    def receive():
                        while b"\n" not in data:
                            if time.monotonic() > deadline:
                                raise TimeoutError("coding turn timed out")
                            try:
                                chunk = client.recv(65536)
                            except socket.timeout:
                                continue
                            if not chunk:
                                raise RuntimeError("frontend disconnected")
                            data.extend(chunk)
                        line, _, rest = data.partition(b"\n")
                        data[:] = rest
                        event = json.loads(line)
                        if event.get("kind") == "appserver":
                            payload = event["payload"]
                            if "method" in payload and "id" in payload:
                                raise RuntimeError("unexpected approval/server request in isolated never-approval test: " + payload["method"])
                            method, parameters = payload.get("method"), payload.get("params", {})
                            if method == "item/completed":
                                item = parameters["item"]
                                items.append(item)
                                if item["type"] in ("commandExecution", "fileChange"):
                                    tool_completed.append(item)
                            elif method == "item/agentMessage/delta":
                                text.append(parameters["delta"])
                            return payload
                        return event
                    def request(method, parameters):
                        nonlocal serial
                        serial += 1
                        client.sendall((json.dumps({"kind": "appserver", "payload": {"jsonrpc": "2.0", "id": serial, "method": method, "params": parameters}}) + "\n").encode())
                        while True:
                            response = receive()
                            if response.get("id") == serial:
                                if "error" in response:
                                    raise RuntimeError(f"{method}: {response['error']}")
                                return response["result"]
                    while True:
                        event = receive()
                        if event.get("kind") == "bridge.provider" and event.get("state") == "ready":
                            break
                    models = request("model/list", {"includeHidden": False})
                    assert any(m["model"] == model for m in models["data"]), "registered model not discovered"
                    thread = request("thread/start", {"model": model, "modelProvider": "anthropic", "cwd": str(project), "approvalPolicy": "never", "sandbox": "workspace-write"})
                    request("turn/start", {"threadId": thread["thread"]["id"], "input": [{"type": "text", "text": "Read calculator.py with a tool. Fix add() to add its arguments using apply_patch. Run python3 -m unittest -q with a tool and report the result. Work only in this project. Do not change the tests."}]})
                    while True:
                        event = receive()
                        if event.get("method") == "turn/completed":
                            assert event["params"]["turn"]["status"] == "completed", event["params"]["turn"].get("error")
                            break
                    assert tool_completed, "no real Codex tool completed"
                    assert any(i["type"] == "fileChange" for i in tool_completed), "patch tool was not exercised"
                    commands = [i for i in tool_completed if i["type"] == "commandExecution"]
                    assert any(i.get("exitCode") == 0 and "return a - b" in i.get("aggregatedOutput", "") for i in commands), "Codex did not read the original source successfully"
                    assert any(i.get("exitCode") == 0 and "python3 -m unittest -q" in i.get("command", "") and "OK" in i.get("aggregatedOutput", "") for i in commands), "Codex did not run the passing unit test"
                    assert (project / "test_calculator.py").read_text() == test_source, "Codex modified the test instead of only fixing the implementation"
                    assert text, "no streamed assistant text"
                    assert any(i["type"] == "agentMessage" and i.get("phase") == "final_answer" and i.get("text") for i in items), "no completed final answer visible when CodexUI hides updates"
                    assert "return a + b" in (project / "calculator.py").read_text(), "project not fixed"
                    subprocess.run([sys.executable, "-m", "unittest", "-q"], cwd=project, check=True)
                    if mock:
                        assert not mock.failures, mock.failures
                        assert len(mock.requests) == 4, "missing inference continuation after tools"
                    print(f"PASS: {'live Claude' if args.live else 'mock Anthropic'} coding turn; {len(tool_completed)} completed tools, patched file, passing test, final streamed answer")
                    client.close()
                except Exception:
                    log.flush(); log.seek(0)
                    # Application logs contain no provider headers; still redact env key defensively.
                    output = log.read()[-12000:]
                    key = env.get("ANTHROPIC_API_KEY", "")
                    print(output.replace(key, "[REDACTED]") if key else output, file=sys.stderr)
                    if mock and mock.failures:
                        print("Native fixture failures:", mock.failures, file=sys.stderr)
                    raise
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
    finally:
        if mock:
            mock.shutdown(); mock.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
