import assert from "node:assert/strict";
import {test} from "node:test";

import {
    ClientConnection,
    CodexBridgeClient,
    CodexWebSocketSubprotocol,
    WebSocketTransport,
} from "../dist/index.js";

class FakeEndpoint {
    sent = [];
    closes = [];
    acceptWrites = true;
    throwOnSend = false;
    throwOnClose = false;

    send(message) {
        if (this.throwOnSend) throw new Error("send failed");
        this.sent.push(message);
        return this.acceptWrites;
    }

    close(reason) {
        this.closes.push(reason);
        if (this.throwOnClose) throw new Error("close failed");
    }
}

class FakeWebSocket {
    protocol = "";
    readyState = 0;
    bufferedAmount = 0;
    binaryType = "blob";
    onopen = null;
    onmessage = null;
    onerror = null;
    onclose = null;
    sent = [];
    closes = [];
    throwOnSend = false;
    throwOnClose = false;

    send(data) {
        if (this.throwOnSend) throw new Error("send failed");
        this.sent.push(data);
    }

    close(code, reason) {
        this.closes.push({code, reason});
        this.readyState = 2;
        if (this.throwOnClose) throw new Error("close failed");
    }

    open(protocol = CodexWebSocketSubprotocol) {
        this.protocol = protocol;
        this.readyState = 1;
        this.onopen?.();
    }

    message(data) {
        this.onmessage?.({data});
    }

    error() {
        this.onerror?.();
    }

    closed(reason = "") {
        this.readyState = 3;
        this.onclose?.({reason});
    }
}

function webSocketHarness(options = {}, callbacks = {}) {
    const sdk = new CodexBridgeClient();
    const connection = new ClientConnection(sdk, callbacks);
    const socket = new FakeWebSocket();
    let requestedUrl;
    let requestedProtocols;
    const transport = new WebSocketTransport(
        connection,
        "ws://localhost:8080/codex",
        {
            ...options,
            createWebSocket: (url, protocols) => {
                requestedUrl = url;
                requestedProtocols = protocols;
                return socket;
            },
        },
    );
    return {
        sdk,
        connection,
        socket,
        transport,
        get requestedUrl() {
            return requestedUrl;
        },
        get requestedProtocols() {
            return requestedProtocols;
        },
    };
}

function establishBridge(harness) {
    harness.socket.open();
    harness.socket.message(JSON.stringify({
        kind: "bridge.connection",
        event: "opened",
        connectionId: "browser-1",
        role: "controller",
    }));
    harness.socket.message(JSON.stringify({
        kind: "bridge.provider",
        state: "ready",
        providerGeneration: 1,
    }));
}

test("ClientConnection matches attach, online, detach, and callback containment", () => {
    const lifecycle = [];
    const sdk = new CodexBridgeClient();
    const connection = new ClientConnection(sdk, {
        onConnected: () => {
            lifecycle.push("connected");
            throw new Error("contained connected callback");
        },
        onDisconnected: () => {
            lifecycle.push("disconnected");
            throw new Error("contained disconnected callback");
        },
        onFailure: (reason) => {
            lifecycle.push(`failure:${reason}`);
            throw new Error("contained failure callback");
        },
    });
    const endpoint = new FakeEndpoint();
    const other = new FakeEndpoint();

    assert.equal(connection.attach(endpoint), true);
    assert.equal(connection.attach(other), false);
    connection.connected(other);
    assert.equal(connection.online, false);
    connection.connected(endpoint);
    connection.connected(endpoint);
    assert.equal(connection.online, true);
    assert.deepEqual(lifecycle, ["connected"]);

    connection.receive(endpoint, {invalid: true});
    connection.receive(endpoint, {invalid: true});
    assert.deepEqual(lifecycle, [
        "connected",
        "failure:bridge client received an invalid envelope",
    ]);
    assert.deepEqual(endpoint.closes, [
        "bridge client received an invalid envelope",
        "bridge client received an invalid envelope",
    ]);

    connection.detach(endpoint, "test transport loss");
    assert.equal(connection.attached, false);
    assert.equal(connection.online, false);
    assert.deepEqual(lifecycle.at(-1), "disconnected");
    connection.detach(endpoint, "duplicate detach");
    assert.equal(lifecycle.filter((entry) => entry === "disconnected").length, 1);
    connection.dispose();
});

test("ClientConnection retires SDK requests once and contains endpoint failures", () => {
    const responses = [];
    const sdk = new CodexBridgeClient();
    const connection = new ClientConnection(sdk);
    const endpoint = new FakeEndpoint();
    connection.attach(endpoint);
    connection.connected(endpoint);
    connection.receive(endpoint, {
        kind: "bridge.connection",
        event: "opened",
        connectionId: "frontend-transport",
        role: "controller",
    });
    connection.receive(endpoint, {
        kind: "bridge.provider",
        state: "ready",
        providerGeneration: 1,
    });
    sdk.request("thread/list", {}, (response) => responses.push(response));
    assert.equal(endpoint.sent.length, 1);

    connection.detach(endpoint, "transport lost");
    assert.equal(responses.length, 1);
    assert.equal(responses[0].error.code, -32020);
    connection.detach(endpoint, "again");
    assert.equal(responses.length, 1);

    const replacement = new FakeEndpoint();
    replacement.throwOnSend = true;
    assert.equal(connection.attach(replacement), true);
    connection.connected(replacement);
    connection.receive(replacement, {
        kind: "bridge.connection",
        event: "opened",
        connectionId: "replacement",
        role: "controller",
    });
    connection.receive(replacement, {
        kind: "bridge.provider",
        state: "ready",
        providerGeneration: 1,
    });
    sdk.request("thread/list", {}, (response) => responses.push(response));
    assert.equal(responses.at(-1).error.code, -32020);

    replacement.throwOnClose = true;
    connection.disconnect("explicit");
    connection.shutdown();
    connection.shutdown();
    connection.dispose();
});

test("browser transport selects codex and carries exact JSON text envelopes", () => {
    const lifecycle = [];
    const harness = webSocketHarness({}, {
        onConnected: () => lifecycle.push("connected"),
        onDisconnected: () => lifecycle.push("disconnected"),
        onFailure: (reason) => lifecycle.push(`failure:${reason}`),
    });

    assert.equal(harness.requestedUrl, "ws://localhost:8080/codex");
    assert.equal(harness.requestedProtocols, CodexWebSocketSubprotocol);
    assert.equal(harness.socket.binaryType, "arraybuffer");
    assert.equal(harness.connection.attached, true);
    assert.equal(harness.connection.online, false);

    establishBridge(harness);
    assert.equal(harness.connection.online, true);
    assert.equal(harness.transport.connected, true);
    assert.deepEqual(lifecycle, ["connected"]);

    let response;
    const id = harness.sdk.request("thread/list", {limit: 2}, (value) => {
        response = value;
    });
    assert.deepEqual(JSON.parse(harness.socket.sent.at(-1)), {
        kind: "appserver",
        payload: {
            jsonrpc: "2.0",
            id,
            method: "thread/list",
            params: {limit: 2},
        },
    });
    harness.socket.message(JSON.stringify({
        kind: "appserver",
        payload: {id, result: {data: []}},
    }));
    assert.deepEqual(response, {id, result: {data: []}});

    harness.socket.closed("remote close");
    assert.equal(harness.connection.attached, false);
    assert.equal(harness.transport.connected, false);
    assert.deepEqual(lifecycle, ["connected", "disconnected"]);
});

test("browser framing rejects binary, invalid, and oversized messages like C++", () => {
    for (const [data, maximumMessageBytes, expectedCode, expectedReason] of [
        [new ArrayBuffer(4), 1024, 1003, "bridge messages must be WebSocket text messages"],
        ["not json", 1024, 1008, "invalid bridge WebSocket JSON message"],
        [JSON.stringify({kind: "bridge.diagnostic", detail: "long"}), 12, 1008, "bridge message exceeds configured maximum"],
    ]) {
        const harness = webSocketHarness({maximumMessageBytes});
        harness.socket.open();
        harness.socket.message(data);
        assert.deepEqual(harness.socket.closes, [{
            code: expectedCode,
            reason: expectedReason,
        }]);
    }
});

test("valid JSON with an invalid bridge envelope reports one connection failure", () => {
    const failures = [];
    const harness = webSocketHarness({}, {
        onFailure: (reason) => failures.push(reason),
    });
    harness.socket.open();
    harness.socket.message(JSON.stringify({invalid: true}));
    harness.socket.message(JSON.stringify({invalid: true}));
    assert.deepEqual(failures, ["bridge client received an invalid envelope"]);
    assert.deepEqual(harness.socket.closes, [{
        code: 1008,
        reason: "bridge client received an invalid envelope",
    }]);
});

test("subprotocol and socket failures are bounded and reported once", () => {
    const failures = [];
    const harness = webSocketHarness({}, {
        onFailure: (reason) => failures.push(reason),
    });
    harness.socket.open("");
    assert.deepEqual(failures, [
        "bridge WebSocket did not negotiate the codex subprotocol",
    ]);
    assert.equal(harness.socket.closes[0].code, 1008);

    const socketFailure = webSocketHarness({}, {
        onFailure: (reason) => failures.push(reason),
    });
    socketFailure.socket.open();
    socketFailure.socket.error();
    socketFailure.socket.error();
    assert.equal(
        failures.filter((reason) => reason === "bridge WebSocket transport failed").length,
        1,
    );
});

test("outbound size and buffered-byte limits reject SDK requests deterministically", () => {
    for (const configure of [
        (harness) => {
            void harness;
            return {maximumMessageBytes: 100};
        },
        (harness) => {
            harness.socket.bufferedAmount = 100;
            return {maximumBufferedBytes: 100};
        },
    ]) {
        const initial = webSocketHarness();
        const options = configure(initial);
        const harness = webSocketHarness(options);
        if (options.maximumBufferedBytes !== undefined) {
            harness.socket.bufferedAmount = options.maximumBufferedBytes;
        }
        establishBridge(harness);
        const responses = [];
        harness.sdk.request(
            "thread/list",
            {cursor: "x".repeat(256)},
            (response) => responses.push(response),
        );
        assert.equal(responses.length, 1);
        assert.equal(responses[0].error.code, -32020);
    }
});

test("shutdown uses the C++ shutdown reason and waits for socket detach", () => {
    const lifecycle = [];
    const harness = webSocketHarness({}, {
        onDisconnected: () => lifecycle.push("disconnected"),
    });
    establishBridge(harness);
    harness.connection.shutdown();
    harness.connection.shutdown();
    assert.deepEqual(harness.socket.closes, [{
        code: 1008,
        reason: "bridge client shutdown",
    }]);
    assert.equal(harness.connection.attached, true);
    harness.socket.closed("bridge client shutdown");
    assert.equal(harness.connection.attached, false);
    assert.deepEqual(lifecycle, ["disconnected"]);
});
