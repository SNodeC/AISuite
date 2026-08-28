import {
    ClientConnection,
    CodexBridgeClient,
    WebSocketTransport,
} from "../src/index.js";
import type {JsonRpcRequest} from "../src/index.js";

const client = new CodexBridgeClient(() => true);

client.request("thread/list", {limit: 10, archived: false}, (response) => {
    if ("result" in response) response.result.data;
});
client.request("account/rateLimits/read", undefined, (response) => {
    if ("error" in response) response.error.code;
});
client.notify("initialized");
client.onServerRequest("item/tool/requestUserInput", (request) => {
    request.params;
});
client.onServerNotification("thread/started", (notification) => {
    notification.params?.thread;
});
client.respond({id: "request", method: "example"} satisfies JsonRpcRequest, {});

const connection = new ClientConnection(client);
const transport = new WebSocketTransport(connection, "ws://localhost:8080/codex");
transport.connected;

// @ts-expect-error thread/list parameters are required.
client.request("thread/list", undefined, () => undefined);
// @ts-expect-error this is a server notification, not a client request.
client.request("thread/started", {}, () => undefined);
// @ts-expect-error only generated client notifications can be emitted.
client.notify("thread/started", {});
