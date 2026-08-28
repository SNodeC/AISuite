import type {ClientConnection, TransportEndpoint} from "./ClientConnection.js";
import type {BridgeMessage} from "./protocol/envelope.js";

export const CodexWebSocketSubprotocol = "codex";
export const DefaultMaximumMessageBytes = 64 * 1024 * 1024;

export interface WebSocketMessageEventLike {
    readonly data: unknown;
}

export interface WebSocketCloseEventLike {
    readonly reason?: string;
}

export interface WebSocketLike {
    readonly protocol: string;
    readonly readyState: number;
    readonly bufferedAmount: number;
    binaryType: BinaryType;
    onopen: (() => void) | null;
    onmessage: ((event: WebSocketMessageEventLike) => void) | null;
    onerror: (() => void) | null;
    onclose: ((event: WebSocketCloseEventLike) => void) | null;
    send(data: string): void;
    close(code?: number, reason?: string): void;
}

export type WebSocketFactory = (
    url: string,
    protocols: string | string[],
) => WebSocketLike;

export interface WebSocketTransportOptions {
    readonly maximumMessageBytes?: number;
    readonly maximumBufferedBytes?: number;
    readonly createWebSocket?: WebSocketFactory;
}

const Connecting = 0;
const Open = 1;
const Closing = 2;
const Closed = 3;
const CloseUnsupportedData = 1003;
const ClosePolicyViolation = 1008;
const WebSocketFramePayloadBytes = 16 * 1024;
const MaximumClientFrameHeaderBytes = 8;

const encoder = new TextEncoder();
const strictDecoder = new TextDecoder("utf-8", {fatal: true});

function byteLength(value: string): number {
    return encoder.encode(value).byteLength;
}

function boundedCloseReason(reason: string): string {
    const bytes = encoder.encode(reason);
    if (bytes.byteLength <= 123) return reason;
    for (let length = 123; length >= 0; --length) {
        try {
            return strictDecoder.decode(bytes.subarray(0, length));
        } catch {
            // Remove the incomplete trailing UTF-8 sequence.
        }
    }
    return "";
}

function framedMessageBytes(payloadBytes: number): number {
    const frames = payloadBytes === 0
        ? 1
        : Math.floor((payloadBytes - 1) / WebSocketFramePayloadBytes) + 1;
    return payloadBytes + frames * MaximumClientFrameHeaderBytes;
}

function defaultFactory(url: string, protocols: string | string[]): WebSocketLike {
    return new WebSocket(url, protocols) as WebSocketLike;
}

export class WebSocketTransport implements TransportEndpoint {
    private readonly socket: WebSocketLike;
    private readonly maximumMessageBytes: number;
    private readonly maximumBufferedBytes: number;
    private detached = false;
    private closing = false;

    public constructor(
        private readonly connection: ClientConnection,
        url: string,
        options: WebSocketTransportOptions = {},
    ) {
        this.maximumMessageBytes = options.maximumMessageBytes
            ?? DefaultMaximumMessageBytes;
        this.maximumBufferedBytes = options.maximumBufferedBytes
            ?? DefaultMaximumMessageBytes;
        if (this.maximumMessageBytes <= 0 || this.maximumBufferedBytes <= 0) {
            throw new RangeError("WebSocket byte limits must be positive");
        }

        const factory = options.createWebSocket ?? defaultFactory;
        this.socket = factory(url, CodexWebSocketSubprotocol);
        this.socket.binaryType = "arraybuffer";
        if (!this.connection.attach(this)) {
            this.closing = true;
            this.socket.close(ClosePolicyViolation, "bridge client already has a transport");
            return;
        }
        this.bindSocket();
    }

    public send(message: BridgeMessage): boolean {
        if (this.closing || this.detached || this.socket.readyState !== Open) {
            return false;
        }
        try {
            const serialized = JSON.stringify(message);
            const bytes = byteLength(serialized);
            if (bytes > this.maximumMessageBytes) return false;
            const queuedBytes = framedMessageBytes(bytes);
            if (
                this.socket.bufferedAmount > this.maximumBufferedBytes
                || queuedBytes > this.maximumBufferedBytes - this.socket.bufferedAmount
            ) {
                return false;
            }
            this.socket.send(serialized);
            return true;
        } catch {
            return false;
        }
    }

    public close(reason: string): void {
        this.closeWithStatus(ClosePolicyViolation, reason);
    }

    public shutdown(): void {
        this.close("bridge client shutdown");
    }

    public get connected(): boolean {
        return !this.detached && !this.closing && this.socket.readyState === Open;
    }

    private bindSocket(): void {
        this.socket.onopen = () => {
            if (this.closing || this.detached) return;
            if (this.socket.protocol !== CodexWebSocketSubprotocol) {
                this.connection.failed(
                    this,
                    "bridge WebSocket did not negotiate the codex subprotocol",
                );
                return;
            }
            this.connection.connected(this);
        };
        this.socket.onmessage = (event) => {
            if (this.closing || this.detached) return;
            if (typeof event.data !== "string") {
                this.closeWithStatus(
                    CloseUnsupportedData,
                    "bridge messages must be WebSocket text messages",
                );
                return;
            }
            if (byteLength(event.data) > this.maximumMessageBytes) {
                this.closeWithStatus(
                    ClosePolicyViolation,
                    "bridge message exceeds configured maximum",
                );
                return;
            }
            let message: unknown;
            try {
                message = JSON.parse(event.data);
            } catch {
                this.closeWithStatus(
                    ClosePolicyViolation,
                    "invalid bridge WebSocket JSON message",
                );
                return;
            }
            this.connection.receive(this, message);
        };
        this.socket.onerror = () => {
            if (!this.detached) {
                this.connection.failed(this, "bridge WebSocket transport failed");
            }
        };
        this.socket.onclose = (event) => {
            this.detach(event.reason || "bridge WebSocket disconnected");
        };
    }

    private closeWithStatus(status: number, reason: string): void {
        if (this.closing || this.detached) return;
        this.closing = true;
        try {
            if (this.socket.readyState !== Closing && this.socket.readyState !== Closed) {
                this.socket.close(status, boundedCloseReason(reason));
            } else if (this.socket.readyState === Closed) {
                this.detach(reason);
            }
        } catch {
            this.detach(reason);
        }
    }

    private detach(reason: string): void {
        if (this.detached) return;
        this.detached = true;
        this.closing = true;
        this.socket.onopen = null;
        this.socket.onmessage = null;
        this.socket.onerror = null;
        this.socket.onclose = null;
        this.connection.detach(this, reason);
    }
}
