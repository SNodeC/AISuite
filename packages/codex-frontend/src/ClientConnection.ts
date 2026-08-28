import type {CodexBridgeClient} from "./CodexBridgeClient.js";
import type {BridgeMessage} from "./protocol/envelope.js";

export interface TransportEndpoint {
    send(message: BridgeMessage): boolean;
    close(reason: string): void;
}

export interface ClientConnectionCallbacks {
    readonly onConnected?: () => void;
    readonly onDisconnected?: () => void;
    readonly onFailure?: (reason: string) => void;
}

export class ClientConnection {
    private endpoint: TransportEndpoint | undefined;
    private isOnline = false;
    private isShuttingDown = false;
    private failureReported = false;

    public constructor(
        private readonly sdk: CodexBridgeClient,
        private readonly callbacks: ClientConnectionCallbacks = {},
    ) {
        this.sdk.setSender((message) => this.send(message));
    }

    public attach(endpoint: TransportEndpoint): boolean {
        if (
            this.isShuttingDown
            || (this.endpoint !== undefined && this.endpoint !== endpoint)
        ) {
            return false;
        }
        this.endpoint = endpoint;
        this.failureReported = false;
        return true;
    }

    public connected(endpoint: TransportEndpoint): void {
        if (this.endpoint !== endpoint || this.isShuttingDown || this.isOnline) return;
        this.isOnline = true;
        this.invokeContained(this.callbacks.onConnected);
    }

    public receive(endpoint: TransportEndpoint, message: unknown): void {
        if (this.endpoint !== endpoint || !this.isOnline || this.isShuttingDown) return;
        try {
            if (!this.sdk.receive(message)) {
                this.failed(endpoint, "bridge client received an invalid envelope");
            }
        } catch {
            this.failed(endpoint, "bridge client failed while dispatching an envelope");
        }
    }

    public failed(endpoint: TransportEndpoint, reason: string): void {
        if (this.endpoint !== endpoint) return;
        this.reportFailure(reason);
        try {
            endpoint.close(reason);
        } catch {
            // Transport close failures cannot escape lifecycle dispatch.
        }
    }

    public detach(endpoint: TransportEndpoint, reason: string): void {
        if (this.endpoint !== endpoint) return;
        this.endpoint = undefined;
        const wasOnline = this.isOnline;
        this.isOnline = false;
        this.failureReported = false;
        this.sdk.transportDisconnected(
            this.isShuttingDown ? "bridge client shutdown" : reason,
        );
        if (wasOnline) this.invokeContained(this.callbacks.onDisconnected);
    }

    public disconnect(reason: string): void {
        if (!this.isShuttingDown && this.endpoint !== undefined) {
            try {
                this.endpoint.close(reason);
            } catch {
                // Explicit disconnect remains noexcept at the application boundary.
            }
        }
    }

    public shutdown(): void {
        if (this.isShuttingDown) return;
        this.isShuttingDown = true;
        if (this.endpoint !== undefined) {
            try {
                this.endpoint.close("bridge client shutdown");
            } catch {
                // Shutdown remains contained even when the transport misbehaves.
            }
        } else {
            this.sdk.transportDisconnected("bridge client shutdown");
        }
    }

    public dispose(): void {
        this.shutdown();
        this.sdk.setSender(undefined);
    }

    public get attached(): boolean {
        return this.endpoint !== undefined;
    }

    public get online(): boolean {
        return this.isOnline;
    }

    private send(message: BridgeMessage): boolean {
        if (!this.isOnline || this.isShuttingDown || this.endpoint === undefined) {
            return false;
        }
        try {
            return this.endpoint.send(message);
        } catch {
            return false;
        }
    }

    private reportFailure(reason: string): void {
        if (this.failureReported) return;
        this.failureReported = true;
        this.invokeContained(this.callbacks.onFailure, reason);
    }

    private invokeContained<Arguments extends readonly unknown[]>(
        callback: ((...args: Arguments) => void) | undefined,
        ...args: Arguments
    ): void {
        if (callback === undefined) return;
        try {
            callback(...args);
        } catch {
            // Application lifecycle callbacks are isolated from the transport.
        }
    }
}
