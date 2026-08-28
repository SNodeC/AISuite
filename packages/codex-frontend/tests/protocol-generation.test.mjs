import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import {test} from "node:test";
import {fileURLToPath} from "node:url";

import {
    clientNotificationOperations,
    clientRequestOperations,
    protocolGeneration,
    serverNotificationOperations,
    serverRequestOperations,
} from "../dist/protocol/generated.js";

const packageDirectory = fileURLToPath(new URL("..", import.meta.url));
const repositoryDirectory = fileURLToPath(new URL("../../..", import.meta.url));
const generatedSource = await readFile(
    new URL("../src/protocol/generated.ts", import.meta.url),
    "utf8",
);
const cppHeader = await readFile(
    `${repositoryDirectory}/src/ai/openai/codex/protocol/generated/ProtocolTypes.h`,
    "utf8",
);
const manifest = JSON.parse(await readFile(
    `${repositoryDirectory}/src/ai/openai/codex/protocol/generated/manifest.json`,
    "utf8",
));

function captured(source, pattern, description) {
    const match = source.match(pattern);
    assert.ok(match?.[1], `missing ${description}`);
    return match[1];
}

function cppTypeName(value) {
    if (value === "Value") return "unknown";
    const match = value.match(/^(root|v2)::([A-Za-z0-9_]+)$/);
    assert.ok(match, `unsupported generated C++ type ${value}`);
    return `${match[1] === "v2" ? "V2" : "Root"}${match[2]}`;
}

function cppDefinitions(namespace) {
    const block = captured(
        cppHeader,
        new RegExp(`namespace ${namespace} \\{\\n([\\s\\S]*?)\\n\\} // namespace ${namespace}`),
        `${namespace} declarations`,
    );
    return [...block.matchAll(/^\s*class\s+([A-Za-z0-9_]+);$/gm)]
        .map((match) => `${namespace === "v2" ? "V2" : "Root"}${match[1]}`)
        .sort();
}

function cppOperations(namespace) {
    const block = captured(
        cppHeader,
        new RegExp(`namespace ${namespace} \\{\\n([\\s\\S]*?)\\n\\} // namespace ${namespace}`),
        `${namespace} operations`,
    );
    const operations = {};
    for (const match of block.matchAll(/struct\s+\w+\s+\{([\s\S]*?)\n\s*\};/g)) {
        const body = match[1];
        const method = captured(body, /method\s*=\s*"([^"]+)";/, "operation method");
        const params = captured(body, /using Params\s*=\s*([^;]+);/, `${method} params`);
        const responseMatch = body.match(/using Response\s*=\s*([^;]+);/);
        const required = captured(body, /paramsRequired\s*=\s*(true|false);/, `${method} paramsRequired`);
        operations[method] = {
            paramsRequired: required === "true",
            paramsType: cppTypeName(params),
            ...(responseMatch ? {responseType: cppTypeName(responseMatch[1])} : {}),
        };
    }
    return operations;
}

test("TypeScript and C++ outputs identify the same protocol sources", () => {
    const schemaHash = captured(cppHeader, /Schema SHA-256: ([a-f0-9]{64})/, "C++ schema hash");
    const sourceHash = captured(cppHeader, /Protocol source SHA-256: ([a-f0-9]{64})/, "C++ protocol source hash");

    assert.equal(protocolGeneration.schemaSha256, schemaHash);
    assert.equal(protocolGeneration.schemaSha256, manifest.schemaSha256);
    assert.equal(protocolGeneration.protocolSourceSha256, sourceHash);
    assert.equal(protocolGeneration.protocolSourceSha256, manifest.protocolSourceSha256);
});

test("TypeScript and C++ expose the same generated type graph", () => {
    const typescriptTypes = [...generatedSource.matchAll(/^export type\s+((?:Root|V2)[A-Za-z0-9_]+)\s*=/gm)]
        .map((match) => match[1])
        .sort();
    const cppTypes = [...cppDefinitions("root"), ...cppDefinitions("v2")].sort();

    assert.deepEqual(typescriptTypes, cppTypes);
    assert.equal(typescriptTypes.length, manifest.generatedTypes);
    assert.equal(protocolGeneration.generatedTypes, manifest.generatedTypes);
    assert.equal(protocolGeneration.canonicalRootTypes, manifest.canonicalRootTypes);
    assert.equal(protocolGeneration.canonicalV2Types, manifest.canonicalV2Types);
});

test("TypeScript and C++ expose equal operation bindings", () => {
    const categories = [
        ["client_requests", clientRequestOperations, manifest.clientRequests],
        ["server_requests", serverRequestOperations, manifest.serverRequests],
        ["client_notifications", clientNotificationOperations, manifest.clientNotifications],
        ["server_notifications", serverNotificationOperations, manifest.serverNotifications],
    ];

    for (const [namespace, typescriptOperations, expectedCount] of categories) {
        assert.deepEqual(typescriptOperations, cppOperations(namespace), namespace);
        assert.equal(Object.keys(typescriptOperations).length, expectedCount, namespace);
    }
});

test("package paths resolve inside the repository", () => {
    assert.ok(packageDirectory.startsWith(repositoryDirectory));
});
