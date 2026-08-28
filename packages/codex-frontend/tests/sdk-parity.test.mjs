import assert from "node:assert/strict";
import {test} from "node:test";

import {
    CodexBridgeClient,
    classifyJsonRpc,
    jsonRpcError,
    jsonRpcIdKey,
} from "../dist/index.js";

function readyClient(sender = () => true) {
    const client = new CodexBridgeClient(sender);
    assert.equal(client.receive({
        kind: "bridge.connection",
        event: "opened",
        connectionId: "frontend-7",
        role: "observer",
        seq: 1,
    }), true);
    assert.equal(client.receive({
        kind: "bridge.controller",
        controllerConnectionId: "frontend-7",
        seq: 2,
    }), true);
    assert.equal(client.receive({
        kind: "bridge.provider",
        state: "ready",
        providerGeneration: 1,
        seq: 3,
    }), true);
    return client;
}

test("JSON-RPC classification matches the permissive C++ frontend classifier", () => {
    assert.equal(classifyJsonRpc({method: "notice", params: {}}), "notification");
    assert.equal(classifyJsonRpc({method: "request", id: 4}), "request");
    assert.equal(classifyJsonRpc({id: "response", result: {}}), "response");
    assert.equal(classifyJsonRpc({id: 2, error: {code: 1}}), "response");
    assert.equal(classifyJsonRpc({id: 2, result: {}, error: {}}), "invalid");
    assert.equal(classifyJsonRpc({id: null, result: {}}), "invalid");
    assert.equal(classifyJsonRpc([]), "invalid");
    assert.equal(jsonRpcIdKey({id: 42}), "42");
    assert.equal(jsonRpcIdKey({id: "42"}), "\"42\"");
    assert.equal(jsonRpcIdKey({id: 4.2}), undefined);
    assert.deepEqual(jsonRpcError("request", -1, "failed", {retry: false}), {
        jsonrpc: "2.0",
        id: "request",
        error: {code: -1, message: "failed", data: {retry: false}},
    });
});

test("bridge telemetry establishes the same identity, role, and provider state", () => {
    const bridgeEvents = [];
    const client = new CodexBridgeClient();
    client.onBridgeEvent((event) => bridgeEvents.push(event));

    assert.equal(client.receive(null), false);
    assert.equal(client.receive({}), false);
    assert.equal(client.receive({kind: "appserver", payload: []}), false);
    readyClient((message) => {
        void message;
        return true;
    });

    client.receive({
        kind: "bridge.connection",
        event: "opened",
        connectionId: "frontend-7",
        role: "observer",
    });
    client.receive({kind: "bridge.controller", controllerConnectionId: "frontend-7"});
    client.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});

    assert.deepEqual(client.snapshot(), {
        connectionId: "frontend-7",
        controllerConnectionId: "frontend-7",
        role: "controller",
        providerGeneration: 1,
        providerReady: true,
    });
    assert.equal(client.isController, true);
    assert.equal(bridgeEvents.length, 3);

    client.onBridgeEvent(() => {
        throw new Error("contained");
    });
    assert.equal(client.receive({kind: "bridge.diagnostic", code: "test"}), true);
});

test("typed request correlation and raw observation equal the C++ SDK", () => {
    const sent = [];
    const raw = [];
    const client = readyClient((message) => {
        sent.push(message);
        return true;
    });
    client.onRawJson((direction, message) => raw.push([direction, message]));

    let response;
    const requestId = client.request(
        "thread/list",
        {limit: 3, archived: false},
        (value) => {
            response = value;
        },
    );
    assert.deepEqual(sent, [{
        kind: "appserver",
        payload: {
            jsonrpc: "2.0",
            id: requestId,
            method: "thread/list",
            params: {limit: 3, archived: false},
        },
    }]);
    assert.equal(requestId, "frontend-7-request-1");

    const payload = {id: requestId, result: {data: [], nextCursor: null}};
    assert.equal(client.receive({kind: "appserver", payload}), true);
    assert.equal(response, payload);
    assert.deepEqual(raw, [
        ["to-app-server", sent[0].payload],
        ["from-app-server", payload],
    ]);

    client.receive({kind: "appserver", payload});
    assert.equal(response, payload, "a duplicate response cannot repeat the retired callback");
});

test("precondition and send failures use the same error codes and exact-once callbacks", () => {
    const responses = [];
    const client = new CodexBridgeClient(() => false);
    const unconnectedId = client.request("thread/list", {}, (response) => responses.push(response));
    assert.equal(unconnectedId, "unconnected-frontend-request-1");
    assert.equal(responses[0].error.code, -32021);

    client.receive({
        kind: "bridge.connection",
        event: "opened",
        connectionId: "frontend-failure",
        role: "controller",
    });
    client.request("thread/list", {}, (response) => responses.push(response));
    assert.equal(responses[1].error.code, -32002);

    client.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    client.request("thread/list", {}, (response) => responses.push(response));
    assert.equal(responses[2].error.code, -32020);
    assert.equal(responses.length, 3);

    client.setSender(() => {
        throw new Error("sender exceptions retain the C++ sender contract");
    });
    assert.throws(() => client.claimController());
});

test("provider generation and transport loss retire callbacks exactly once", () => {
    const responses = [];
    const client = readyClient();
    client.request("thread/list", {}, (response) => responses.push(response));
    client.request("thread/list", {}, () => {
        throw new Error("contained lifecycle callback");
    });

    client.receive({
        kind: "bridge.provider",
        state: "disconnected",
        providerGeneration: 1,
        reason: "test provider restart",
    });
    assert.equal(responses.length, 1);
    assert.equal(responses[0].error.code, -32002);
    assert.equal(responses[0].error.message, "test provider restart");
    assert.equal(client.providerReady, false);

    client.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    assert.equal(client.providerGeneration, 2);
    assert.equal(client.providerReady, true);
    client.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    assert.equal(client.providerGeneration, 2, "stale provider telemetry is ignored");

    client.request("thread/list", {}, (response) => responses.push(response));
    client.transportDisconnected("test transport loss");
    assert.equal(responses.length, 2);
    assert.equal(responses[1].error.code, -32020);
    assert.equal(responses[1].error.message, "test transport loss");
    assert.deepEqual(client.snapshot(), {
        providerGeneration: 0,
        providerReady: false,
    });
    client.transportDisconnected("duplicate detach");
    assert.equal(responses.length, 2);
});

test("notifications, controller commands, and raw messages preserve envelopes", () => {
    const sent = [];
    const client = readyClient((message) => {
        sent.push(message);
        return true;
    });

    assert.equal(client.notify("initialized"), true);
    assert.equal(client.claimController(), true);
    assert.equal(client.releaseController(), true);
    assert.equal(client.transferController("frontend-9"), true);
    assert.equal(client.sendRawJson({method: "thread/started", params: {thread: {id: "t"}}}), true);
    assert.equal(client.sendRawJson({invalid: true}), false);

    assert.deepEqual(sent, [
        {kind: "appserver", payload: {jsonrpc: "2.0", method: "initialized"}},
        {kind: "bridge.controller", action: "claim"},
        {kind: "bridge.controller", action: "release"},
        {kind: "bridge.controller", action: "transfer", targetConnectionId: "frontend-9"},
        {kind: "appserver", payload: {method: "thread/started", params: {thread: {id: "t"}}}},
    ]);
});

test("server request and notification dispatch match C++ error behavior", () => {
    const sent = [];
    const client = readyClient((message) => {
        sent.push(message);
        return true;
    });

    const missing = {
        id: 71,
        method: "item/tool/requestUserInput",
        params: {threadId: "thread"},
    };
    client.receive({kind: "appserver", payload: missing});
    assert.deepEqual(sent.pop(), {
        kind: "appserver",
        payload: {
            jsonrpc: "2.0",
            id: 71,
            error: {
                code: -32601,
                message: "frontend has no handler for the server request",
            },
        },
    });

    client.onServerRequest("item/tool/requestUserInput", () => {
        throw new Error("handler failed");
    });
    client.receive({kind: "appserver", payload: missing});
    assert.equal(sent.at(-1).payload.error.code, -32603);
    assert.equal(sent.at(-1).payload.error.message, "frontend request handler failed");

    client.onServerRequest("item/tool/requestUserInput", (request) => {
        assert.equal(client.respond(request, {answers: {}}), true);
    });
    client.receive({kind: "appserver", payload: missing});
    assert.deepEqual(sent.at(-1), {
        kind: "appserver",
        payload: {jsonrpc: "2.0", id: 71, result: {answers: {}}},
    });

    let notifications = 0;
    client.onServerNotification("thread/started", (notification) => {
        assert.equal(notification.params.thread.id, "thread");
        ++notifications;
    });
    client.receive({
        kind: "appserver",
        payload: {method: "thread/started", params: {thread: {id: "thread"}}},
    });
    assert.equal(notifications, 1);
    client.onServerNotification("thread/started", () => {
        ++notifications;
        throw new Error("contained notification callback");
    });
    assert.equal(client.receive({
        kind: "appserver",
        payload: {method: "thread/started", params: {thread: {id: "thread"}}},
    }), true);
    assert.equal(notifications, 2);
});

test("Promise requests resolve to the same complete response envelope", async () => {
    let sent;
    const client = readyClient((message) => {
        sent = message;
        return true;
    });
    const responsePromise = client.requestPromise("thread/read", {
        threadId: "thread",
        includeTurns: true,
    });
    client.receive({
        kind: "appserver",
        payload: {id: sent.payload.id, result: {thread: {id: "thread"}}},
    });
    assert.deepEqual(await responsePromise, {
        id: sent.payload.id,
        result: {thread: {id: "thread"}},
    });
});
