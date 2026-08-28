#!/usr/bin/env node

// Generates lossless C++ views and optional structural TypeScript declarations
// for every named and anonymous Codex app-server schema node. The OpenAI schema
// is input only; this script writes exclusively to paths passed by the caller.

import fs from "node:fs";
import crypto from "node:crypto";

if (process.argv.length !== 6 && process.argv.length !== 7) {
    throw new Error("usage: generate-codex-protocol.mjs SCHEMA PROTOCOL_SOURCE OUTPUT_HEADER OUTPUT_MANIFEST [OUTPUT_TYPESCRIPT]");
}

const [schemaPath, protocolSourcePath, outputPath, manifestPath, typescriptOutputPath] = process.argv.slice(2);
const schemaText = fs.readFileSync(schemaPath, "utf8");
const schema = JSON.parse(schemaText);
const schemaSha256 = crypto.createHash("sha256").update(schemaText).digest("hex");
const protocolSourceText = fs.readFileSync(protocolSourcePath, "utf8");
const protocolSourceSha256 = crypto.createHash("sha256").update(protocolSourceText).digest("hex");

function sourceLabel(value) {
    const marker = "/codex-rs/";
    const markerIndex = value.lastIndexOf(marker);
    return markerIndex < 0 ? value : `codex-rs/${value.slice(markerIndex + marker.length)}`;
}

const cppKeywords = new Set([
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
    "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do",
    "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
    "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
    "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
]);

function words(value) {
    return String(value)
        .replace(/::/g, " ")
        .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
        .replace(/[^A-Za-z0-9]+/g, " ")
        .trim()
        .split(/\s+/)
        .filter(Boolean);
}

function pascal(value) {
    const result = words(value).map((part) => part[0].toUpperCase() + part.slice(1)).join("");
    const safe = result || "Anonymous";
    return /^[0-9]/.test(safe) ? `N${safe}` : safe;
}

function camel(value) {
    const name = pascal(value);
    const result = name[0].toLowerCase() + name.slice(1);
    return cppKeywords.has(result) ? `${result}_` : result;
}

function cppString(value) {
    return JSON.stringify(String(value));
}

function refTarget(ref) {
    const prefix = "#/definitions/";
    if (!ref.startsWith(prefix)) {
        throw new Error(`unsupported external schema ref: ${ref}`);
    }
    const segments = ref.slice(prefix.length).split("/");
    if (segments[0] === "v2") {
        return {ns: "v2", name: pascal(segments.slice(1).join("_"))};
    }
    return {ns: "root", name: pascal(segments.join("_"))};
}

function qualified(type, currentNs) {
    if (type.ns === currentNs) {
        return type.name;
    }
    return type.ns === "v2" ? `v2::${type.name}` : `root::${type.name}`;
}

function nodeType(node) {
    const value = node?.type;
    if (Array.isArray(value)) {
        return value.find((entry) => entry !== "null") ?? "null";
    }
    return value;
}

function isComplex(node) {
    if (!node || node.$ref) return false;
    const type = nodeType(node);
    return Boolean(node.oneOf || node.anyOf || node.allOf || node.enum || type === "object" || type === "array" || node.properties || node.items);
}

const definitions = [];
const byKey = new Map();
const usedNames = {root: new Set(), v2: new Set()};

function uniqueName(ns, preferred) {
    const base = pascal(preferred);
    let candidate = base;
    let suffix = 2;
    while (usedNames[ns].has(candidate)) {
        candidate = `${base}${suffix++}`;
    }
    usedNames[ns].add(candidate);
    return candidate;
}

function addDefinition(ns, preferredName, node, key, canonical = false) {
    if (byKey.has(key)) return byKey.get(key);
    const name = canonical ? pascal(preferredName) : uniqueName(ns, preferredName);
    const definition = {ns, name, node, key, children: new Map()};
    definitions.push(definition);
    byKey.set(key, definition);
    collectChildren(definition);
    return definition;
}

function reserveCanonicalNames(ns, entries) {
    for (const [schemaName] of entries) {
        const name = pascal(schemaName);
        if (usedNames[ns].has(name)) {
            throw new Error(`canonical schema names collide as C++ type ${ns}::${name}`);
        }
        usedNames[ns].add(name);
    }
}

function collectChildren(definition) {
    const {node, ns, name, key} = definition;
    for (const [propertyName, propertyNode] of Object.entries(node.properties ?? {})) {
        if (isComplex(propertyNode)) {
            const child = addDefinition(ns, `${name}${pascal(propertyName)}`, propertyNode, `${key}/properties/${propertyName}`);
            definition.children.set(`property:${propertyName}`, child);
        }
    }
    if (isComplex(node.items)) {
        const child = addDefinition(ns, `${name}Item`, node.items, `${key}/items`);
        definition.children.set("items", child);
    }
    for (const unionName of ["oneOf", "anyOf", "allOf"]) {
        for (const [index, variant] of (node[unionName] ?? []).entries()) {
            if (isComplex(variant)) {
                const title = variant.title ? pascal(variant.title) : `${name}${pascal(unionName)}${index + 1}`;
                const child = addDefinition(ns, title, variant, `${key}/${unionName}/${index}`);
                definition.children.set(`${unionName}:${index}`, child);
            }
        }
    }
}

const rootSchemaDefinitions = Object.entries(schema.definitions ?? {}).filter(([name]) => name !== "v2");
const v2SchemaDefinitions = Object.entries(schema.definitions?.v2 ?? {});

reserveCanonicalNames("root", rootSchemaDefinitions);
reserveCanonicalNames("v2", v2SchemaDefinitions);

for (const [name, node] of rootSchemaDefinitions) {
    if (name !== "v2") addDefinition("root", name, node, `#/definitions/${name}`, true);
}
for (const [name, node] of v2SchemaDefinitions) {
    addDefinition("v2", name, node, `#/definitions/v2/${name}`, true);
}

function typeForNode(node, definition, location, currentNs) {
    if (node?.$ref) return qualified(refTarget(node.$ref), currentNs);
    const child = definition.children.get(location);
    if (child) return qualified(child, currentNs);
    switch (nodeType(node)) {
        case "string": return "std::optional<std::string>";
        case "integer": return "std::optional<std::int64_t>";
        case "number": return "std::optional<double>";
        case "boolean": return "std::optional<bool>";
        case "null": return "nlohmann::json";
        default: return "nlohmann::json";
    }
}

function accessorExpression(node, definition, location, jsonName, currentNs) {
    if (node?.$ref || definition.children.has(location)) {
        return `${typeForNode(node, definition, location, currentNs)}(memberRaw(${cppString(jsonName)}))`;
    }
    switch (nodeType(node)) {
        case "string": return `stringMember(${cppString(jsonName)})`;
        case "integer": return `integerMember(${cppString(jsonName)})`;
        case "number": return `numberMember(${cppString(jsonName)})`;
        case "boolean": return `boolMember(${cppString(jsonName)})`;
        default: return `memberRaw(${cppString(jsonName)})`;
    }
}

function valueType(definition) {
    const {node, ns} = definition;
    if (node.$ref) return qualified(refTarget(node.$ref), ns);
    switch (nodeType(node)) {
        case "string": return "std::optional<std::string>";
        case "integer": return "std::optional<std::int64_t>";
        case "number": return "std::optional<double>";
        case "boolean": return "std::optional<bool>";
        default: return "nlohmann::json";
    }
}

function itemType(definition) {
    const item = definition.node.items ?? {};
    if (item.$ref) return qualified(refTarget(item.$ref), definition.ns);
    const child = definition.children.get("items");
    if (child) return qualified(child, definition.ns);
    switch (nodeType(item)) {
        case "string": return "std::string";
        case "integer": return "std::int64_t";
        case "number": return "double";
        case "boolean": return "bool";
        default: return "nlohmann::json";
    }
}

function classDeclaration(definition) {
    const lines = [];
    lines.push(`    class ${definition.name} final : public Value {`);
    lines.push("    public:");
    lines.push("        using Value::Value;");
    const type = nodeType(definition.node);
    if (type === "object" || definition.node.properties) {
        for (const [propertyName, propertyNode] of Object.entries(definition.node.properties ?? {})) {
            lines.push(`        ${typeForNode(propertyNode, definition, `property:${propertyName}`, definition.ns)} ${camel(propertyName)}() const;`);
        }
        lines.push("        std::vector<std::string> keys() const;");
    } else if (type === "array") {
        lines.push(`        std::vector<${itemType(definition)}> items() const;`);
    } else if (definition.node.oneOf || definition.node.anyOf || definition.node.allOf) {
        const count = (definition.node.oneOf ?? definition.node.anyOf ?? definition.node.allOf).length;
        lines.push(`        static constexpr std::size_t variantCount = ${count};`);
    } else {
        lines.push(`        ${valueType(definition)} value() const;`);
    }
    if (definition.node.enum) {
        lines.push("        bool isKnown() const;");
        lines.push("        static const std::vector<nlohmann::json>& knownValues();");
    }
    lines.push("    };");
    return lines.join("\n");
}

function classDefinitions(definition) {
    const prefix = `${definition.name}::`;
    const lines = [];
    const type = nodeType(definition.node);
    if (type === "object" || definition.node.properties) {
        for (const [propertyName, propertyNode] of Object.entries(definition.node.properties ?? {})) {
            const returnType = typeForNode(propertyNode, definition, `property:${propertyName}`, definition.ns);
            const expression = accessorExpression(propertyNode, definition, `property:${propertyName}`, propertyName, definition.ns);
            lines.push(`    inline ${returnType} ${prefix}${camel(propertyName)}() const { return ${expression}; }`);
        }
        lines.push(`    inline std::vector<std::string> ${prefix}keys() const { return objectKeys(); }`);
    } else if (type === "array") {
        const item = itemType(definition);
        lines.push(`    inline std::vector<${item}> ${prefix}items() const { return arrayItems<${item}>(); }`);
    } else if (!(definition.node.oneOf || definition.node.anyOf || definition.node.allOf)) {
        const returnType = valueType(definition);
        let expression = "getRaw()";
        if (returnType === "std::optional<std::string>") expression = "stringValue()";
        if (returnType === "std::optional<std::int64_t>") expression = "integerValue()";
        if (returnType === "std::optional<double>") expression = "numberValue()";
        if (returnType === "std::optional<bool>") expression = "boolValue()";
        if (definition.node.$ref) expression = `${returnType}(getRaw())`;
        lines.push(`    inline ${returnType} ${prefix}value() const { return ${expression}; }`);
    }
    if (definition.node.enum) {
        const values = definition.node.enum.map((entry) => `nlohmann::json(${JSON.stringify(entry)})`).join(", ");
        lines.push(`    inline const std::vector<nlohmann::json>& ${prefix}knownValues() { static const std::vector<nlohmann::json> values{${values}}; return values; }`);
        lines.push(`    inline bool ${prefix}isKnown() const { const auto& values = knownValues(); return std::find(values.begin(), values.end(), getRaw()) != values.end(); }`);
    }
    return lines.join("\n");
}

function findDefinition(refOrName, preferredNs = "v2") {
    if (refOrName?.startsWith?.("#/")) {
        const target = refTarget(refOrName);
        return definitions.find((entry) => entry.ns === target.ns && entry.name === target.name);
    }
    const name = pascal(refOrName);
    return definitions.find((entry) => entry.ns === preferredNs && entry.name === name) ??
        definitions.find((entry) => entry.ns === "root" && entry.name === name);
}

function macroInvocationBody(name) {
    const invocation = protocolSourceText.indexOf(`${name}!`);
    if (invocation < 0) throw new Error(`missing ${name}! invocation in protocol source`);
    const start = protocolSourceText.indexOf("{", invocation);
    if (start < 0) throw new Error(`missing opening brace for ${name}! invocation`);

    let depth = 0;
    let quote = null;
    let escaped = false;
    let lineComment = false;
    let blockComment = false;
    for (let index = start; index < protocolSourceText.length; ++index) {
        const character = protocolSourceText[index];
        const next = protocolSourceText[index + 1];
        if (lineComment) {
            if (character === "\n") lineComment = false;
            continue;
        }
        if (blockComment) {
            if (character === "*" && next === "/") {
                blockComment = false;
                ++index;
            }
            continue;
        }
        if (quote !== null) {
            if (escaped) {
                escaped = false;
            } else if (character === "\\") {
                escaped = true;
            } else if (character === quote) {
                quote = null;
            }
            continue;
        }
        if (character === "/" && next === "/") {
            lineComment = true;
            ++index;
            continue;
        }
        if (character === "/" && next === "*") {
            blockComment = true;
            ++index;
            continue;
        }
        if (character === '"' || character === "'") {
            quote = character;
            continue;
        }
        if (character === "{") {
            ++depth;
        } else if (character === "}" && --depth === 0) {
            return protocolSourceText.slice(start + 1, index);
        }
    }
    throw new Error(`unterminated ${name}! invocation`);
}

function requestResponseBindings(macroName) {
    const body = macroInvocationBody(macroName);
    const bindings = new Map();
    const entryPattern = /(?:^|\n)\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=>\s*"([^"]+)")?\s*\{([\s\S]*?)\n\s*\},/g;
    for (const match of body.matchAll(entryPattern)) {
        const variant = match[1];
        const wireMethod = match[2] ?? `${variant[0].toLowerCase()}${variant.slice(1)}`;
        const responseMatch = match[3].match(/(?:^|\n)\s*response:\s*([A-Za-z_][A-Za-z0-9_:]*)\s*,/);
        if (!responseMatch) throw new Error(`${macroName} ${variant} has no parseable response type`);
        const rustType = responseMatch[1];
        const separator = rustType.indexOf("::");
        const rustNamespace = separator < 0 ? "root" : rustType.slice(0, separator);
        const typeName = separator < 0 ? rustType : rustType.slice(separator + 2);
        const schemaNamespace = rustNamespace === "v2" ? "v2" : "root";
        bindings.set(wireMethod, {variant, responseNamespace: schemaNamespace, responseName: pascal(typeName), rustType});
    }
    return bindings;
}

function operationsFrom(unionName, direction, responseBindings = null) {
    const union = schema.definitions?.[unionName];
    const result = [];
    for (const [index, variant] of (union?.oneOf ?? []).entries()) {
        const method = variant.properties?.method?.enum?.[0];
        if (!method) throw new Error(`${unionName} variant ${index} has no literal method`);
        const title = variant.title ?? `${pascal(method)}${direction}`;
        const base = title.replace(/Request$/, "").replace(/Notification$/, "");
        const paramsRef = variant.properties?.params?.$ref;
        const params = paramsRef ? findDefinition(paramsRef) : null;
        const binding = responseBindings?.get(method);
        const response = direction === "Request" && binding
            ? definitions.find((entry) => entry.ns === binding.responseNamespace && entry.name === binding.responseName) ??
                definitions.find((entry) => entry.name === binding.responseName)
            : null;
        if (direction === "Request" && !response) {
            const detail = binding ? `response ${binding.rustType} is absent from the exported schema` : "source binding is absent";
            throw new Error(`no response binding for ${unionName} method ${method}: ${detail}`);
        }
        result.push({
            name: uniqueName("root", binding?.variant ?? base),
            method,
            params,
            paramsRequired: (variant.required ?? []).includes("params"),
            response,
            direction
        });
    }
    return result;
}

const clientRequestBindings = requestResponseBindings("client_request_definitions");
const serverRequestBindings = requestResponseBindings("server_request_definitions");
const clientRequests = operationsFrom("ClientRequest", "Request", clientRequestBindings);
const serverRequests = operationsFrom("ServerRequest", "Request", serverRequestBindings);
const clientNotifications = operationsFrom("ClientNotification", "Notification");
const serverNotifications = operationsFrom("ServerNotification", "Notification");

function cppType(definition) {
    if (!definition) return "Value";
    return definition.ns === "v2" ? `v2::${definition.name}` : `root::${definition.name}`;
}

function operationDeclaration(operation) {
    const lines = [`    struct ${operation.name} {`, `        static constexpr std::string_view method = ${cppString(operation.method)};`];
    lines.push(`        using Params = ${cppType(operation.params)};`);
    if (operation.response) lines.push(`        using Response = ${cppType(operation.response)};`);
    lines.push(`        static constexpr bool paramsRequired = ${operation.paramsRequired ? "true" : "false"};`);
    lines.push("    };");
    return lines.join("\n");
}

function operationMacro(name, operations) {
    if (operations.length === 0) return `#define ${name}(X)\n`;
    const entries = operations.map((operation) => `    X(${operation.name}, ${camel(operation.name)})`);
    return `#define ${name}(X) \\\n${entries.join(" \\\n")}\n`;
}

function typescriptName(definition) {
    if (!definition) return "unknown";
    return `${definition.ns === "v2" ? "V2" : "Root"}${definition.name}`;
}

function typescriptRef(ref) {
    const target = refTarget(ref);
    return `${target.ns === "v2" ? "V2" : "Root"}${target.name}`;
}

function typescriptLiteral(value) {
    if (value === null) return "null";
    if (typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
        return JSON.stringify(value);
    }
    return "unknown";
}

function typescriptType(node, definition, location = null) {
    if (!node || typeof node !== "object") return "unknown";
    if (node.$ref) return typescriptRef(node.$ref);

    const child = location === null ? null : definition.children.get(location);
    if (child) return typescriptName(child);

    if (Object.hasOwn(node, "const")) return typescriptLiteral(node.const);
    if (Array.isArray(node.enum) && node.enum.length > 0) {
        return node.enum.map(typescriptLiteral).join(" | ");
    }

    for (const unionName of ["oneOf", "anyOf"]) {
        if (Array.isArray(node[unionName]) && node[unionName].length > 0) {
            return node[unionName]
                .map((variant, index) => typescriptType(variant, definition, `${unionName}:${index}`))
                .join(" | ");
        }
    }
    if (Array.isArray(node.allOf) && node.allOf.length > 0) {
        return node.allOf
            .map((variant, index) => typescriptType(variant, definition, `allOf:${index}`))
            .join(" & ");
    }

    if (Array.isArray(node.type)) {
        return node.type
            .map((type) => typescriptType({...node, type}, definition, location))
            .join(" | ");
    }

    const type = nodeType(node);
    if (type === "string") return "string";
    if (type === "integer" || type === "number") return "number";
    if (type === "boolean") return "boolean";
    if (type === "null") return "null";
    if (type === "array") {
        const item = node.items ? typescriptType(node.items, definition, "items") : "unknown";
        return `ReadonlyArray<${item}>`;
    }
    if (type === "object" || node.properties || node.additionalProperties) {
        const required = new Set(node.required ?? []);
        const properties = Object.entries(node.properties ?? {}).map(([name, property]) => {
            const optional = required.has(name) ? "" : "?";
            return `    readonly ${JSON.stringify(name)}${optional}: ${typescriptType(property, definition, `property:${name}`)};`;
        });
        if (node.additionalProperties !== false) {
            properties.push("    readonly [key: string]: unknown;");
        }
        if (properties.length === 0) {
            return node.additionalProperties === false ? "Record<string, never>" : "Record<string, unknown>";
        }
        return `{\n${properties.join("\n")}\n}`;
    }
    return "unknown";
}

function typescriptDefinition(definition) {
    return `export type ${typescriptName(definition)} = ${typescriptType(definition.node, definition)};`;
}

function typescriptOperationMap(name, operations) {
    const lines = operations.map((operation) => {
        const fields = [
            `readonly params: ${typescriptName(operation.params)};`,
            `readonly paramsRequired: ${operation.paramsRequired ? "true" : "false"};`
        ];
        if (operation.response) fields.splice(1, 0, `readonly response: ${typescriptName(operation.response)};`);
        return `    readonly ${JSON.stringify(operation.method)}: { ${fields.join(" ")} };`;
    });
    return `export interface ${name} {\n${lines.join("\n")}\n}`;
}

function typescriptOperationMetadata(name, operations) {
    const entries = operations.map((operation) => {
        const fields = [
            `paramsRequired: ${operation.paramsRequired ? "true" : "false"}`,
            `paramsType: ${JSON.stringify(typescriptName(operation.params))}`
        ];
        if (operation.response) fields.push(`responseType: ${JSON.stringify(typescriptName(operation.response))}`);
        return `    ${JSON.stringify(operation.method)}: {${fields.join(", ")}},`;
    });
    return `export const ${name} = {\n${entries.join("\n")}\n} as const;`;
}

const rootDefinitions = definitions.filter((entry) => entry.ns === "root");
const v2Definitions = definitions.filter((entry) => entry.ns === "v2");
let output = `/*\n * Generated from Codex app-server protocol exports. DO NOT EDIT.\n * Schema SHA-256: ${schemaSha256}\n * Protocol source SHA-256: ${protocolSourceSha256}\n * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT\n */\n\n`;
output += `#ifndef AI_OPENAI_CODEX_GENERATED_PROTOCOLTYPES_H\n#define AI_OPENAI_CODEX_GENERATED_PROTOCOLTYPES_H\n\n`;
output += `#include <algorithm>\n#include <cstddef>\n#include <cstdint>\n#include <optional>\n#include <string>\n#include <string_view>\n#include <type_traits>\n#include <utility>\n#include <vector>\n#include <nlohmann/json.hpp>\n\n`;
output += `namespace ai::openai::codex::generated {\n\n`;
output += `    class Value {\n    public:\n        Value() = default;\n        explicit Value(nlohmann::json raw) : raw_(std::move(raw)) {}\n        const nlohmann::json& getRaw() const noexcept { return raw_; }\n        const nlohmann::json& getPayload() const noexcept { return payloadRaw(); }\n        bool isJsonRpcResponse() const noexcept { if (!raw_.is_object()) return false; const bool hasResult = raw_.contains("result"); const bool hasError = raw_.contains("error"); return raw_.contains("id") && !raw_["id"].is_null() && hasResult != hasError; }\n        bool ok() const noexcept { return isJsonRpcResponse() ? raw_.contains("result") : !raw_.is_null() && !raw_.is_discarded(); }\n        explicit operator bool() const noexcept { return ok(); }\n        const nlohmann::json& jsonRpcId() const noexcept { if (!raw_.is_object()) return nullJson(); const auto iterator = raw_.find("id"); return iterator == raw_.end() ? nullJson() : *iterator; }\n        std::optional<std::string> jsonRpcMethod() const { if (!raw_.is_object()) return std::nullopt; const auto iterator = raw_.find("method"); return iterator != raw_.end() && iterator->is_string() ? std::optional<std::string>(iterator->get<std::string>()) : std::nullopt; }\n        std::optional<std::int64_t> jsonRpcErrorCode() const { if (!raw_.is_object()) return std::nullopt; const auto error = raw_.find("error"); if (error == raw_.end() || !error->is_object()) return std::nullopt; const auto code = error->find("code"); return code != error->end() && code->is_number_integer() ? std::optional<std::int64_t>(code->get<std::int64_t>()) : std::nullopt; }\n        std::optional<std::string> jsonRpcErrorMessage() const { if (!raw_.is_object()) return std::nullopt; const auto error = raw_.find("error"); if (error == raw_.end() || !error->is_object()) return std::nullopt; const auto message = error->find("message"); return message != error->end() && message->is_string() ? std::optional<std::string>(message->get<std::string>()) : std::nullopt; }\n    protected:\n        const nlohmann::json& payloadRaw() const noexcept { if (isJsonRpcResponse()) { const auto result = raw_.find("result"); if (result != raw_.end()) return *result; } if (raw_.is_object() && raw_.contains("method")) { const auto params = raw_.find("params"); if (params != raw_.end()) return *params; } return raw_; }\n        nlohmann::json memberRaw(std::string_view name) const { const nlohmann::json& payload = payloadRaw(); if (!payload.is_object()) return nullptr; const auto it = payload.find(std::string(name)); return it == payload.end() ? nlohmann::json(nullptr) : *it; }\n        std::optional<std::string> stringMember(std::string_view name) const { return optionalValue<std::string>(memberRaw(name)); }\n        std::optional<std::int64_t> integerMember(std::string_view name) const { return optionalValue<std::int64_t>(memberRaw(name)); }\n        std::optional<double> numberMember(std::string_view name) const { const auto value = memberRaw(name); return value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt; }\n        std::optional<bool> boolMember(std::string_view name) const { return optionalValue<bool>(memberRaw(name)); }\n        std::optional<std::string> stringValue() const { return optionalValue<std::string>(payloadRaw()); }\n        std::optional<std::int64_t> integerValue() const { return optionalValue<std::int64_t>(payloadRaw()); }\n        std::optional<double> numberValue() const { const nlohmann::json& payload = payloadRaw(); return payload.is_number() ? std::optional<double>(payload.get<double>()) : std::nullopt; }\n        std::optional<bool> boolValue() const { return optionalValue<bool>(payloadRaw()); }\n        std::vector<std::string> objectKeys() const { const nlohmann::json& payload = payloadRaw(); std::vector<std::string> result; if (payload.is_object()) { result.reserve(payload.size()); for (const auto& [key, value] : payload.items()) { static_cast<void>(value); result.push_back(key); } } return result; }\n        template <typename T> std::vector<T> arrayItems() const { const nlohmann::json& payload = payloadRaw(); std::vector<T> result; if (!payload.is_array()) return result; result.reserve(payload.size()); for (const auto& item : payload) { if constexpr (std::is_same_v<T, nlohmann::json>) result.push_back(item); else if constexpr (std::is_base_of_v<Value, T>) result.emplace_back(item); else if (item.is_null()) continue; else { try { result.push_back(item.get<T>()); } catch (const nlohmann::json::exception&) {} } } return result; }\n    private:\n        static const nlohmann::json& nullJson() noexcept { static const nlohmann::json value = nullptr; return value; }\n        template <typename T> static std::optional<T> optionalValue(const nlohmann::json& value) { if (value.is_null()) return std::nullopt; try { return value.get<T>(); } catch (const nlohmann::json::exception&) { return std::nullopt; } }\n        nlohmann::json raw_ = nullptr;\n    };\n\n`;

output += "namespace root {\n" + rootDefinitions.map((entry) => `    class ${entry.name};`).join("\n") + "\n} // namespace root\n\n";
output += "namespace v2 {\n" + v2Definitions.map((entry) => `    class ${entry.name};`).join("\n") + "\n} // namespace v2\n\n";
output += "namespace root {\n" + rootDefinitions.map(classDeclaration).join("\n\n") + "\n} // namespace root\n\n";
output += "namespace v2 {\n" + v2Definitions.map(classDeclaration).join("\n\n") + "\n} // namespace v2\n\n";
output += "namespace root {\n" + rootDefinitions.map(classDefinitions).filter(Boolean).join("\n") + "\n} // namespace root\n\n";
output += "namespace v2 {\n" + v2Definitions.map(classDefinitions).filter(Boolean).join("\n") + "\n} // namespace v2\n\n";
output += "namespace client_requests {\n" + clientRequests.map(operationDeclaration).join("\n\n") + "\n} // namespace client_requests\n\n";
output += "namespace server_requests {\n" + serverRequests.map(operationDeclaration).join("\n\n") + "\n} // namespace server_requests\n\n";
output += "namespace client_notifications {\n" + clientNotifications.map(operationDeclaration).join("\n\n") + "\n} // namespace client_notifications\n\n";
output += "namespace server_notifications {\n" + serverNotifications.map(operationDeclaration).join("\n\n") + "\n} // namespace server_notifications\n\n";
output += `} // namespace ai::openai::codex::generated\n\n`;
output += operationMacro("AI_OPENAI_CODEX_CLIENT_REQUESTS", clientRequests);
output += operationMacro("AI_OPENAI_CODEX_PARAMETERLESS_CLIENT_REQUESTS", clientRequests.filter((operation) => !operation.paramsRequired));
output += operationMacro("AI_OPENAI_CODEX_SERVER_REQUESTS", serverRequests);
output += operationMacro("AI_OPENAI_CODEX_CLIENT_NOTIFICATIONS", clientNotifications);
output += operationMacro("AI_OPENAI_CODEX_PARAMETERLESS_CLIENT_NOTIFICATIONS", clientNotifications.filter((operation) => !operation.paramsRequired));
output += operationMacro("AI_OPENAI_CODEX_SERVER_NOTIFICATIONS", serverNotifications);
output += "\n#endif\n";

fs.writeFileSync(outputPath, output);
const manifest = {
    schema: sourceLabel(schemaPath),
    schemaSha256,
    protocolSource: sourceLabel(protocolSourcePath),
    protocolSourceSha256,
    generatedTypes: definitions.length,
    canonicalRootTypes: rootDefinitions.filter((entry) => entry.key.split("/").length === 3).length,
    canonicalV2Types: v2Definitions.filter((entry) => entry.key.split("/").length === 4).length,
    clientRequests: clientRequests.length,
    serverRequests: serverRequests.length,
    clientNotifications: clientNotifications.length,
    serverNotifications: serverNotifications.length
};
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");

if (typescriptOutputPath) {
    let typescript = `/*\n * Generated from Codex app-server protocol exports. DO NOT EDIT.\n * Schema SHA-256: ${schemaSha256}\n * Protocol source SHA-256: ${protocolSourceSha256}\n * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT\n */\n\n`;
    typescript += `export const protocolGeneration = {\n`;
    typescript += `    schemaSha256: ${JSON.stringify(schemaSha256)},\n`;
    typescript += `    protocolSourceSha256: ${JSON.stringify(protocolSourceSha256)},\n`;
    typescript += `    generatedTypes: ${definitions.length},\n`;
    typescript += `    canonicalRootTypes: ${manifest.canonicalRootTypes},\n`;
    typescript += `    canonicalV2Types: ${manifest.canonicalV2Types},\n`;
    typescript += `} as const;\n\n`;
    typescript += definitions.map(typescriptDefinition).join("\n\n") + "\n\n";
    typescript += typescriptOperationMap("ClientRequestMap", clientRequests) + "\n\n";
    typescript += typescriptOperationMap("ServerRequestMap", serverRequests) + "\n\n";
    typescript += typescriptOperationMap("ClientNotificationMap", clientNotifications) + "\n\n";
    typescript += typescriptOperationMap("ServerNotificationMap", serverNotifications) + "\n\n";
    typescript += typescriptOperationMetadata("clientRequestOperations", clientRequests) + "\n\n";
    typescript += typescriptOperationMetadata("serverRequestOperations", serverRequests) + "\n\n";
    typescript += typescriptOperationMetadata("clientNotificationOperations", clientNotifications) + "\n\n";
    typescript += typescriptOperationMetadata("serverNotificationOperations", serverNotifications) + "\n";
    fs.writeFileSync(typescriptOutputPath, typescript);
}
console.log(JSON.stringify(manifest));
