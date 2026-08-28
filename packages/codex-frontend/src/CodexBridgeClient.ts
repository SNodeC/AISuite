import type {
    ClientNotificationMap,
    ClientRequestMap,
    ServerNotificationMap,
    ServerRequestMap,
} from "./protocol/generated.js";
import {
    classifyJsonRpc,
    isObject,
    jsonRpcError,
    jsonRpcIdKey,
    jsonRpcMethod,
} from "./protocol/envelope.js";
import type {
    AppServerDirection,
    AppServerMessage,
    BridgeMessage,
    JsonRpcFailure,
    JsonRpcId,
    JsonRpcRequest,
    JsonRpcResponse,
    Role,
} from "./protocol/envelope.js";

export type Sender = (message: BridgeMessage) => boolean;
export type RawHandler = (
    direction: AppServerDirection,
    message: AppServerMessage,
) => void;
export type BridgeEventHandler = (message: BridgeMessage) => void;

type ClientRequestMethod = keyof ClientRequestMap;
type ClientNotificationMethod = keyof ClientNotificationMap;
type ServerRequestMethod = keyof ServerRequestMap;
type ServerNotificationMethod = keyof ServerNotificationMap;

type RequestResult<Method extends ClientRequestMethod> = JsonRpcResponse<
    ClientRequestMap[Method]["response"]
>;

type RequestHandler<Method extends ClientRequestMethod> = (
    response: RequestResult<Method>,
) => void;

type RequestArguments<Method extends ClientRequestMethod> =
    ClientRequestMap[Method]["paramsRequired"] extends true
        ? readonly [params: ClientRequestMap[Method]["params"], handler: RequestHandler<Method>]
        : readonly [params: ClientRequestMap[Method]["params"] | undefined, handler: RequestHandler<Method>];

type PendingHandler = (response: JsonRpcResponse<unknown>) => void;
type MessageHandler = (message: AppServerMessage) => void;

export interface BridgeSnapshot {
    readonly connectionId?: string;
    readonly controllerConnectionId?: string;
    readonly role?: Role;
    readonly providerGeneration: number;
    readonly providerReady: boolean;
}

export class CodexBridgeClient {
    private sender: Sender | undefined;
    private rawHandler: RawHandler | undefined;
    private bridgeEventHandler: BridgeEventHandler | undefined;
    private pending = new Map<string, PendingHandler>();
    private readonly serverRequestHandlers = new Map<string, MessageHandler>();
    private readonly serverNotificationHandlers = new Map<string, MessageHandler>();
    private connectionIdentity: string | undefined;
    private controllerIdentity: string | undefined;
    private currentRole: Role | undefined;
    private currentProviderGeneration = 0;
    private currentProviderReady = false;
    private nextRequestNumber = 1;

    public constructor(sender?: Sender) {
        this.sender = sender;
    }

    public setSender(sender?: Sender): void {
        this.sender = sender;
    }

    public receive(bridgeMessage: unknown): boolean {
        if (!isObject(bridgeMessage) || typeof bridgeMessage.kind !== "string") {
            return false;
        }
        if (bridgeMessage.kind !== "appserver") {
            this.updateBridgeState(bridgeMessage);
            this.invokeContained(this.bridgeEventHandler, bridgeMessage);
            return true;
        }

        if (!isObject(bridgeMessage.payload)) return false;
        const payload = bridgeMessage.payload;
        this.invokeContained(this.rawHandler, "from-app-server", payload);

        const messageKind = classifyJsonRpc(payload);
        const id = jsonRpcIdKey(payload);
        if (messageKind === "response" && id !== undefined) {
            const handler = this.pending.get(id);
            if (handler !== undefined) {
                this.pending.delete(id);
                this.invokeContained(
                    handler,
                    payload as unknown as JsonRpcResponse<unknown>,
                );
            }
            return true;
        }

        const method = jsonRpcMethod(payload);
        if (messageKind === "request" && method !== undefined) {
            const handler = this.serverRequestHandlers.get(method);
            if (handler !== undefined) {
                try {
                    handler(payload);
                } catch {
                    if (id !== undefined) {
                        this.sendServerError(
                            payload.id as JsonRpcId,
                            -32603,
                            "frontend request handler failed",
                        );
                    }
                }
            } else if (id !== undefined) {
                this.sendServerError(
                    payload.id as JsonRpcId,
                    -32601,
                    "frontend has no handler for the server request",
                );
            }
            return true;
        }
        if (messageKind === "notification" && method !== undefined) {
            this.invokeContained(this.serverNotificationHandlers.get(method), payload);
            return true;
        }
        return messageKind !== "invalid";
    }

    public sendRawJson(message: AppServerMessage): boolean {
        return classifyJsonRpc(message) !== "invalid" && this.sendAppServerMessage(message);
    }

    public onRawJson(handler?: RawHandler): void {
        this.rawHandler = handler;
    }

    public onBridgeEvent(handler?: BridgeEventHandler): void {
        this.bridgeEventHandler = handler;
    }

    public transportDisconnected(reason: string): void {
        this.failPending(reason, -32020);
        this.connectionIdentity = undefined;
        this.controllerIdentity = undefined;
        this.currentRole = undefined;
        this.currentProviderGeneration = 0;
        this.currentProviderReady = false;
    }

    public request<Method extends ClientRequestMethod>(
        method: Method,
        ...[params, handler]: RequestArguments<Method>
    ): string {
        return this.requestTyped(
            method,
            params,
            handler as PendingHandler,
        );
    }

    public requestPromise<Method extends ClientRequestMethod>(
        method: Method,
        params: ClientRequestMap[Method]["params"] | undefined,
    ): Promise<RequestResult<Method>> {
        return new Promise((resolve) => {
            this.requestTyped(method, params, resolve as PendingHandler);
        });
    }

    public notify<Method extends ClientNotificationMethod>(
        method: Method,
        params?: ClientNotificationMap[Method]["params"],
    ): boolean {
        const notification: Record<string, unknown> = {jsonrpc: "2.0", method};
        if (params !== undefined) notification.params = params;
        return this.connectionIdentity !== undefined && this.sendAppServerMessage(notification);
    }

    public onServerRequest<Method extends ServerRequestMethod>(
        method: Method,
        handler?: (request: JsonRpcRequest<ServerRequestMap[Method]["params"]>) => void,
    ): void {
        this.registerHandler(this.serverRequestHandlers, method, handler as MessageHandler | undefined);
    }

    public onServerNotification<Method extends ServerNotificationMethod>(
        method: Method,
        handler?: (notification: AppServerMessage & {
            readonly params?: ServerNotificationMap[Method]["params"];
        }) => void,
    ): void {
        this.registerHandler(this.serverNotificationHandlers, method, handler as MessageHandler | undefined);
    }

    public respond(request: JsonRpcRequest<unknown>, result: unknown): boolean {
        return this.sendServerResponse(request.id, result);
    }

    public respondError(
        request: JsonRpcRequest<unknown>,
        code: number,
        message: string,
        data: unknown = null,
    ): boolean {
        return this.sendServerError(request.id, code, message, data);
    }

    public claimController(): boolean {
        return this.sendBridgeCommand({kind: "bridge.controller", action: "claim"});
    }

    public releaseController(): boolean {
        return this.sendBridgeCommand({kind: "bridge.controller", action: "release"});
    }

    public transferController(targetConnectionId: string): boolean {
        return this.sendBridgeCommand({
            kind: "bridge.controller",
            action: "transfer",
            targetConnectionId,
        });
    }

    public get connectionId(): string | undefined {
        return this.connectionIdentity;
    }

    public get controllerConnectionId(): string | undefined {
        return this.controllerIdentity;
    }

    public get role(): Role | undefined {
        return this.currentRole;
    }

    public get isController(): boolean {
        return this.currentRole === "controller";
    }

    public get providerGeneration(): number {
        return this.currentProviderGeneration;
    }

    public get providerReady(): boolean {
        return this.currentProviderReady;
    }

    public snapshot(): BridgeSnapshot {
        return {
            ...(this.connectionIdentity === undefined ? {} : {connectionId: this.connectionIdentity}),
            ...(this.controllerIdentity === undefined
                ? {}
                : {controllerConnectionId: this.controllerIdentity}),
            ...(this.currentRole === undefined ? {} : {role: this.currentRole}),
            providerGeneration: this.currentProviderGeneration,
            providerReady: this.currentProviderReady,
        };
    }

    private requestTyped(
        method: string,
        params: unknown,
        handler: PendingHandler | undefined,
    ): string {
        const id = this.nextRequestId();
        const key = JSON.stringify(id);
        const request: Record<string, unknown> = {jsonrpc: "2.0", id, method};
        if (params !== undefined) request.params = params;

        if (this.connectionIdentity === undefined || !this.currentProviderReady) {
            this.invokeContained(
                handler,
                jsonRpcError(
                    id,
                    this.connectionIdentity === undefined ? -32021 : -32002,
                    this.connectionIdentity === undefined
                        ? "frontend bridge connection is not established"
                        : "app-server provider is not ready",
                ),
            );
            return id;
        }

        if (handler !== undefined) this.pending.set(key, handler);
        else this.pending.set(key, () => undefined);
        if (!this.sendAppServerMessage(request)) {
            const failed = this.pending.get(key);
            this.pending.delete(key);
            this.invokeContained(
                failed,
                jsonRpcError(id, -32020, "frontend bridge transport rejected request"),
            );
        }
        return id;
    }

    private sendServerResponse(id: JsonRpcId | null, result: unknown): boolean {
        return id === null
            ? false
            : this.sendAppServerMessage({jsonrpc: "2.0", id, result});
    }

    private sendServerError(
        id: JsonRpcId | null,
        code: number,
        message: string,
        data: unknown = null,
    ): boolean {
        return id === null
            ? false
            : this.sendAppServerMessage(
                jsonRpcError(id, code, message, data) as unknown as AppServerMessage,
            );
    }

    private sendAppServerMessage(message: AppServerMessage): boolean {
        if (
            this.connectionIdentity === undefined
            || this.sender === undefined
            || !this.sender({kind: "appserver", payload: message})
        ) {
            return false;
        }
        this.invokeContained(this.rawHandler, "to-app-server", message);
        return true;
    }

    private sendBridgeCommand(command: BridgeMessage): boolean {
        return this.connectionIdentity !== undefined
            && this.sender !== undefined
            && this.sender(command);
    }

    private registerHandler(
        handlers: Map<string, MessageHandler>,
        method: string,
        handler: MessageHandler | undefined,
    ): void {
        if (handler === undefined) handlers.delete(method);
        else handlers.set(method, handler);
    }

    private updateBridgeState(message: Record<string, unknown>): void {
        const stringMember = (name: string): string =>
            typeof message[name] === "string" ? message[name] : "";

        if (message.kind === "bridge.connection" && stringMember("event") === "opened") {
            if (typeof message.connectionId === "string") {
                this.connectionIdentity = message.connectionId;
            }
            const role = stringMember("role");
            this.currentRole = role === "controller" || role === "observer" ? role : undefined;
            return;
        }
        if (message.kind === "bridge.controller") {
            this.controllerIdentity = typeof message.controllerConnectionId === "string"
                ? message.controllerConnectionId
                : undefined;
            if (this.connectionIdentity !== undefined) {
                this.currentRole = this.controllerIdentity === this.connectionIdentity
                    ? "controller"
                    : "observer";
            }
            return;
        }
        if (message.kind !== "bridge.provider") return;

        const generation = message.providerGeneration;
        if (typeof generation !== "number" || !Number.isInteger(generation) || generation < 0) {
            return;
        }
        if (generation < this.currentProviderGeneration) return;
        if (this.currentProviderGeneration !== 0 && generation > this.currentProviderGeneration) {
            this.failPending("app-server provider generation changed", -32002);
        }
        this.currentProviderGeneration = generation;
        const state = stringMember("state");
        this.currentProviderReady = state === "ready";
        if (state === "disconnected") {
            this.failPending(
                stringMember("reason") || "app-server disconnected",
                -32002,
            );
        }
    }

    private failPending(reason: string, code: number): void {
        const pending = this.pending;
        this.pending = new Map();
        for (const [key, handler] of pending) {
            let id: JsonRpcId | null = null;
            try {
                const parsed: unknown = JSON.parse(key);
                if (typeof parsed === "string" || typeof parsed === "number") id = parsed;
            } catch {
                // Invalid keys retain a null error id, matching the C++ proxy.
            }
            this.invokeContained(handler, jsonRpcError(id, code, reason));
        }
    }

    private nextRequestId(): string {
        const prefix = this.connectionIdentity ?? "unconnected-frontend";
        return `${prefix}-request-${this.nextRequestNumber++}`;
    }

    private invokeContained<Arguments extends readonly unknown[]>(
        handler: ((...args: Arguments) => void) | undefined,
        ...args: Arguments
    ): void {
        if (handler === undefined) return;
        try {
            handler(...args);
        } catch {
            // User callbacks never escape protocol dispatch or lifecycle teardown.
        }
    }
}

export type {JsonRpcFailure};
