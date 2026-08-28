export type Role = "controller" | "observer";
export type AppServerDirection = "to-app-server" | "from-app-server";
export type JsonRpcKind = "request" | "notification" | "response" | "invalid";
export type JsonRpcId = string | number;

export interface JsonRpcRequest<Params = unknown> {
    readonly jsonrpc?: "2.0";
    readonly id: JsonRpcId;
    readonly method: string;
    readonly params?: Params;
    readonly [key: string]: unknown;
}

export interface JsonRpcNotification<Params = unknown> {
    readonly jsonrpc?: "2.0";
    readonly method: string;
    readonly params?: Params;
    readonly [key: string]: unknown;
}

export interface JsonRpcSuccess<Result = unknown> {
    readonly jsonrpc?: "2.0";
    readonly id: JsonRpcId;
    readonly result: Result;
}

export interface JsonRpcErrorDetail {
    readonly code: number;
    readonly message: string;
    readonly data?: unknown;
}

export interface JsonRpcFailure {
    readonly jsonrpc: "2.0";
    readonly id: JsonRpcId | null;
    readonly error: JsonRpcErrorDetail;
}

export type JsonRpcResponse<Result = unknown> = JsonRpcSuccess<Result> | JsonRpcFailure;
export type AppServerMessage = Readonly<Record<string, unknown>>;
export type BridgeMessage = Readonly<Record<string, unknown>>;

export function isObject(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function classifyJsonRpc(message: unknown): JsonRpcKind {
    if (!isObject(message)) return "invalid";
    const hasMethod = typeof message.method === "string";
    const hasId = Object.hasOwn(message, "id") && message.id !== null;
    const hasResult = Object.hasOwn(message, "result");
    const hasError = Object.hasOwn(message, "error");
    if (hasMethod) return hasId ? "request" : "notification";
    if (hasId && hasResult !== hasError) return "response";
    return "invalid";
}

export function jsonRpcIdKey(message: unknown): string | undefined {
    if (!isObject(message) || !Object.hasOwn(message, "id") || message.id === null) {
        return undefined;
    }
    if (typeof message.id === "string") return JSON.stringify(message.id);
    if (typeof message.id === "number" && Number.isInteger(message.id)) {
        return JSON.stringify(message.id);
    }
    return undefined;
}

export function jsonRpcMethod(message: unknown): string | undefined {
    return isObject(message) && typeof message.method === "string"
        ? message.method
        : undefined;
}

export function jsonRpcError(
    id: JsonRpcId | null,
    code: number,
    message: string,
    data: unknown = null,
): JsonRpcFailure {
    const error: {code: number; message: string; data?: unknown} = {code, message};
    if (data !== null) error.data = data;
    return {jsonrpc: "2.0", id, error};
}
