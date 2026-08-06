#!/usr/bin/env python3
"""Focused negative checks for the reviewed C++ frontend client authority."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = ROOT / "tools/frontend/generate_cpp_frontend_client.py"
BINDINGS = ROOT / "tools/frontend/cpp-client-bindings.json"
def load_generator():
    spec = importlib.util.spec_from_file_location("cpp_frontend_client_generator", GENERATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load C++ frontend client generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def must_fail(module, bindings, expected: str) -> None:
    try:
        module.validate(bindings)
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(f"expected {expected!r} in {error!r}") from error
        return
    raise AssertionError(f"invalid authority unexpectedly passed: {expected}")


def must_fail_provider_schema(module, method, descriptor, operation, schema, expected: str) -> None:
    try:
        module.validate_provider_schema_authority(method, descriptor, operation, schema)
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(f"expected {expected!r} in {error!r}") from error
        return
    raise AssertionError(f"invalid provider schema authority unexpectedly passed: {expected}")


def main() -> int:
    module = load_generator()
    authority = json.loads(BINDINGS.read_text(encoding="utf-8"))
    if authority["formatVersion"] != 2:
        raise AssertionError("the typed C++ binding authority must use format version 2")
    bindings = authority["bindings"]
    module.validate(bindings)
    provider_start = next(index for index, binding in enumerate(bindings) if binding["methodId"] == "ProviderStart")

    manifest = module.load_json(module.MANIFEST)
    schema = module.load_json(module.SCHEMA)
    descriptors = module.provider_descriptors()
    operations = module.provider_operations()
    provider_methods = {
        method["id"]: method for method in manifest["methods"] if method["category"] == "provider_operation"
    }
    if len(provider_methods) != 86:
        raise AssertionError(f"expected 86 provider methods, got {len(provider_methods)}")
    for method_id, method in provider_methods.items():
        module.validate_provider_schema_authority(
            method, descriptors[method_id], operations[method_id], schema
        )

    thread_fork = provider_methods["ThreadFork"]
    wrong_provider_method = dict(thread_fork)
    wrong_provider_method["providerMethod"] = "thread/fork/drift"
    must_fail_provider_schema(
        module,
        wrong_provider_method,
        descriptors["ThreadFork"],
        operations["ThreadFork"],
        schema,
        "provider method disagrees",
    )

    wrong_codec_descriptor = descriptors["ThreadFork"]._replace(provider_method="thread/fork/drift")
    must_fail_provider_schema(
        module,
        thread_fork,
        wrong_codec_descriptor,
        operations["ThreadFork"],
        schema,
        "provider method disagrees",
    )

    wrong_provider_operation = operations["ThreadFork"]._replace(result_type="ThreadReadResponse")
    must_fail_provider_schema(
        module,
        thread_fork,
        descriptors["ThreadFork"],
        wrong_provider_operation,
        schema,
        "result type disagrees",
    )

    wrong_schema_identity = dict(thread_fork)
    wrong_schema_identity["resultSchema"] = "#/$defs/ThreadReadResult"
    must_fail_provider_schema(
        module,
        wrong_schema_identity,
        descriptors["ThreadFork"],
        operations["ThreadFork"],
        schema,
        "frontend resultSchema disagrees",
    )

    wrong_result_schema = copy.deepcopy(schema)
    wrong_result_schema["$defs"]["ThreadForkResult"]["title"] = "ThreadReadResponse"
    must_fail_provider_schema(
        module,
        thread_fork,
        descriptors["ThreadFork"],
        operations["ThreadFork"],
        wrong_result_schema,
        "frontend result schema title disagrees",
    )

    parameter_aliases = {
        method_id
        for method_id, descriptor in descriptors.items()
        if descriptor.parameter_type != f"{method_id}Params"
        and schema["$defs"][f"{method_id}Params"].get("title") is not None
    }
    if not parameter_aliases:
        raise AssertionError("expected the provider inventory to exercise typed parameter aliases")
    for method_id in parameter_aliases:
        wrong_alias_schema = copy.deepcopy(schema)
        wrong_alias_schema["$defs"][f"{method_id}Params"]["title"] = f"{method_id}Params"
        must_fail_provider_schema(
            module,
            provider_methods[method_id],
            descriptors[method_id],
            operations[method_id],
            wrong_alias_schema,
            "frontend parameter schema title disagrees",
        )

    for method_id in module.PROJECTED_PROVIDER_RESULTS:
        wrong_projection = dict(provider_methods[method_id])
        wrong_projection["legacyCompatibilityMethod"] = False
        must_fail_provider_schema(
            module,
            wrong_projection,
            descriptors[method_id],
            operations[method_id],
            schema,
            "projected client result",
        )

    wrong_unit_schema = copy.deepcopy(schema)
    wrong_unit_schema["$defs"]["TurnInterruptResult"]["type"] = "array"
    must_fail_provider_schema(
        module,
        provider_methods["TurnInterrupt"],
        descriptors["TurnInterrupt"],
        operations["TurnInterrupt"],
        wrong_unit_schema,
        "Unit result schema must remain an object",
    )

    must_fail(module, bindings[:-1], "missing MethodIds")
    must_fail(module, bindings + [dict(bindings[0])], "duplicate MethodId")

    wrong_facade = [dict(binding) for binding in bindings]
    wrong_facade[0]["facade"] = "Threads"
    must_fail(module, wrong_facade, "wrong facade")

    wrong_category = [dict(binding) for binding in bindings]
    wrong_category[0]["category"] = "provider"
    must_fail(module, wrong_category, "wrong category")

    wrong_operation = [dict(binding) for binding in bindings]
    wrong_operation[provider_start]["operation"] = "inventedOperation"
    must_fail(module, wrong_operation, "wrong operation")

    generic_parameter = [dict(binding) for binding in bindings]
    generic_parameter[provider_start]["parameterType"] = (
        "frontend::generated::MethodParameters<frontend::generated::MethodId::ProviderStart>"
    )
    must_fail(module, generic_parameter, "generated JSON wrapper")

    generic_result = [dict(binding) for binding in bindings]
    generic_result[provider_start]["resultType"] = (
        "frontend::generated::MethodResult<frontend::generated::MethodId::ProviderStart>"
    )
    must_fail(module, generic_result, "generated JSON wrapper")

    wrong_parameter_header = [dict(binding) for binding in bindings]
    wrong_parameter_header[provider_start]["parameterHeader"] = "ai/openai/codex/typed/Threads.h"
    must_fail(module, wrong_parameter_header, "wrong parameterHeader")

    wrong_result_header = [dict(binding) for binding in bindings]
    wrong_result_header[provider_start]["resultHeader"] = "ai/openai/codex/typed/Threads.h"
    must_fail(module, wrong_result_header, "wrong resultHeader")

    wrong_encoder = [dict(binding) for binding in bindings]
    wrong_encoder[provider_start]["parameterEncoder"] = "missingEncoder"
    must_fail(module, wrong_encoder, "wrong parameterEncoder")

    wrong_decoder = [dict(binding) for binding in bindings]
    wrong_decoder[provider_start]["resultDecoder"] = "missingDecoder"
    must_fail(module, wrong_decoder, "wrong resultDecoder")

    wrong_sensitivity = [dict(binding) for binding in bindings]
    wrong_sensitivity[provider_start]["sensitive"] = True
    must_fail(module, wrong_sensitivity, "wrong sensitive")

    non_boolean_sensitivity = [dict(binding) for binding in bindings]
    non_boolean_sensitivity[provider_start]["sensitive"] = "false"
    must_fail(module, non_boolean_sensitivity, "sensitivity must be boolean")

    return 0


if __name__ == "__main__":
    sys.exit(main())
