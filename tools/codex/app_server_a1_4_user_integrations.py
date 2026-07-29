#!/usr/bin/env python3
"""Freeze and verify the Codex A1.4 user-facing integration batch.

This audit is deliberately independent from the production registry state
after the batch starts.  It reads the predecessor variants from the requested
base commit and the schema closure from the frozen native-A1.4 plan, then
freezes the exact six-commit ownership plan for PR A.  The production registry
remains the sole runtime/status authority.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

sys.dont_write_bytecode = True


FORMAT_VERSION = 1
CODEX_VERSION = "codex-cli 0.144.6"
UPSTREAM_TAG = "rust-v0.144.6"
UPSTREAM_SOURCE_COMMIT = "5d1fbf26c43abc65a203928b2e31561cb039e06d"
EXPECTED_BASE_SHA = "10d3829958a6a17e7437326b6c42c51f3a8de4ec"
EXPECTED_BASE_TREE = "7b5e6500780f1c633fe18af5fba6164bd222a3ba"
EXPECTED_SNODEC_SOURCE = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
EXPECTED_SNODEC_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"
EXPECTED_SOVERSION = 1
EXPECTED_CANONICAL_VARIANT_HASH = (
    "be081aa1374db3958fef5ca30f364e10fe0237a5daafc857900df28b75dd6c86"
)
EXPECTED_EVENT_VARIANT_HASH = (
    "9bdb33fbe20797a16b76785e3f463dc728ee78956ce00c05a193f1c7f29acb77"
)

REQUEST_CONTRACTS = {
    "app/list": ("AppsListParams", "AppsListResponse", "Concrete"),
    "externalAgentConfig/detect": (
        "ExternalAgentConfigDetectParams",
        "ExternalAgentConfigDetectResponse",
        "Concrete",
    ),
    "externalAgentConfig/import": (
        "ExternalAgentConfigImportParams",
        "ExternalAgentConfigImportResponse",
        "Concrete",
    ),
    "externalAgentConfig/import/readHistories": (
        "Unit",
        "ExternalAgentConfigImportHistoriesReadResponse",
        "Concrete",
    ),
    "feedback/upload": (
        "FeedbackUploadParams",
        "FeedbackUploadResponse",
        "Concrete",
    ),
    "hooks/list": ("HooksListParams", "HooksListResponse", "Concrete"),
    "marketplace/add": (
        "MarketplaceAddParams",
        "MarketplaceAddResponse",
        "Concrete",
    ),
    "marketplace/remove": (
        "MarketplaceRemoveParams",
        "MarketplaceRemoveResponse",
        "Concrete",
    ),
    "marketplace/upgrade": (
        "MarketplaceUpgradeParams",
        "MarketplaceUpgradeResponse",
        "Concrete",
    ),
    "plugin/install": (
        "PluginInstallParams",
        "PluginInstallResponse",
        "Concrete",
    ),
    "plugin/installed": (
        "PluginInstalledParams",
        "PluginInstalledResponse",
        "Concrete",
    ),
    "plugin/list": ("PluginListParams", "PluginListResponse", "Concrete"),
    "plugin/read": ("PluginReadParams", "PluginReadResponse", "Concrete"),
    "plugin/share/checkout": (
        "PluginShareCheckoutParams",
        "PluginShareCheckoutResponse",
        "Concrete",
    ),
    "plugin/share/delete": ("PluginShareDeleteParams", "Unit", "Unit"),
    "plugin/share/list": (
        "PluginShareListParams",
        "PluginShareListResponse",
        "Concrete",
    ),
    "plugin/share/save": (
        "PluginShareSaveParams",
        "PluginShareSaveResponse",
        "Concrete",
    ),
    "plugin/share/updateTargets": (
        "PluginShareUpdateTargetsParams",
        "PluginShareUpdateTargetsResponse",
        "Concrete",
    ),
    "plugin/skill/read": (
        "PluginSkillReadParams",
        "PluginSkillReadResponse",
        "Concrete",
    ),
    "plugin/uninstall": ("PluginUninstallParams", "Unit", "Unit"),
    "skills/config/write": (
        "SkillsConfigWriteParams",
        "SkillsConfigWriteResponse",
        "Concrete",
    ),
    "skills/extraRoots/set": ("SkillsExtraRootsSetParams", "Unit", "Unit"),
    "skills/list": ("SkillsListParams", "SkillsListResponse", "Concrete"),
}

NOTIFICATIONS = (
    "app/list/updated",
    "externalAgentConfig/import/completed",
    "externalAgentConfig/import/progress",
    "hook/completed",
    "hook/started",
    "skills/changed",
)
PLUGIN_SOURCE_ORDER = ("git", "local", "npm", "remote")

STAGES = (
    {
        "commit": 2,
        "subject": "Complete Codex apps, external agents, and feedback",
        "requests": (
            "app/list",
            "externalAgentConfig/detect",
            "externalAgentConfig/import",
            "externalAgentConfig/import/readHistories",
            "feedback/upload",
        ),
        "notifications": NOTIFICATIONS[:3],
        "unions": (),
        "native": {"Complete": 8, "Partial": 1, "NotImplemented": 47},
        "global": {
            "Complete": 288,
            "Partial": 4,
            "NotImplemented": 47,
            "NotApplicable": 48,
        },
    },
    {
        "commit": 3,
        "subject": "Complete Codex hooks, marketplace, and skills",
        "requests": (
            "hooks/list",
            "marketplace/add",
            "marketplace/remove",
            "marketplace/upgrade",
            "skills/config/write",
            "skills/extraRoots/set",
            "skills/list",
        ),
        "notifications": NOTIFICATIONS[3:],
        "unions": (),
        "native": {"Complete": 18, "Partial": 1, "NotImplemented": 37},
        "global": {
            "Complete": 298,
            "Partial": 4,
            "NotImplemented": 37,
            "NotApplicable": 48,
        },
    },
    {
        "commit": 4,
        "subject": "Complete Codex plugin operations without source unions",
        "requests": (
            "plugin/install",
            "plugin/share/checkout",
            "plugin/share/delete",
            "plugin/share/save",
            "plugin/share/updateTargets",
            "plugin/skill/read",
            "plugin/uninstall",
        ),
        "notifications": (),
        "unions": (),
        "native": {"Complete": 25, "Partial": 1, "NotImplemented": 30},
        "global": {
            "Complete": 305,
            "Partial": 4,
            "NotImplemented": 30,
            "NotApplicable": 48,
        },
    },
    {
        "commit": 5,
        "subject": "Complete Codex plugin source and catalog operations",
        "requests": (
            "plugin/installed",
            "plugin/list",
            "plugin/read",
            "plugin/share/list",
        ),
        "notifications": (),
        "unions": PLUGIN_SOURCE_ORDER,
        "native": {"Complete": 33, "Partial": 1, "NotImplemented": 22},
        "global": {
            "Complete": 313,
            "Partial": 4,
            "NotImplemented": 22,
            "NotApplicable": 48,
        },
    },
)

PUBLIC_API = {
    "Apps": {
        "accessor": "apps",
        "header": "ai/openai/codex/typed/Apps.h",
        "methods": ("list",),
    },
    "ExternalAgents": {
        "accessor": "externalAgents",
        "header": "ai/openai/codex/typed/ExternalAgents.h",
        "methods": (
            "detect",
            "importConfiguration",
            "readImportHistories",
        ),
    },
    "Feedback": {
        "accessor": "feedback",
        "header": "ai/openai/codex/typed/Feedback.h",
        "methods": ("upload",),
    },
    "Hooks": {
        "accessor": "hooks",
        "header": "ai/openai/codex/typed/Hooks.h",
        "methods": ("list",),
    },
    "Marketplace": {
        "accessor": "marketplace",
        "header": "ai/openai/codex/typed/Marketplace.h",
        "methods": ("add", "remove", "upgrade"),
    },
    "Plugins": {
        "accessor": "plugins",
        "header": "ai/openai/codex/typed/Plugins.h",
        "methods": (
            "install",
            "installed",
            "list",
            "read",
            "shareCheckout",
            "shareDelete",
            "shareList",
            "shareSave",
            "shareUpdateTargets",
            "readSkill",
            "uninstall",
        ),
    },
    "Skills": {
        "accessor": "skills",
        "header": "ai/openai/codex/typed/Skills.h",
        "methods": ("writeConfig", "setExtraRoots", "list"),
    },
}

CODEC_UNITS = (
    "detail/AppCodec.h",
    "detail/AppCodec.cpp",
    "detail/ExternalAgentCodec.h",
    "detail/ExternalAgentCodec.cpp",
    "detail/FeedbackCodec.h",
    "detail/FeedbackCodec.cpp",
    "detail/HookCodec.h",
    "detail/HookCodec.cpp",
    "detail/MarketplaceCodec.h",
    "detail/MarketplaceCodec.cpp",
    "detail/PluginCodec.h",
    "detail/PluginCodec.cpp",
    "detail/SkillCodec.h",
    "detail/SkillCodec.cpp",
)

EXPECTED_CLOSURE = {
    "seed_definitions": 52,
    "reachable_named_definitions": 118,
    "definition_namespaces": {"v2": 118},
    "schema_paths": 411,
    "schema_path_kinds": {
        "array_element": 68,
        "map_value": 4,
        "property": 339,
    },
    "required_paths": 176,
    "optional_paths": 163,
    "nullable_paths": 141,
    "default_bearing_paths": 19,
    "object_nodes": 103,
    "open_objects": 99,
    "closed_objects": 0,
    "schema_valued_additional_properties": 4,
    "opaque_json_paths": 0,
    "sensitive_paths": 66,
}

APPENDS = (
    ("AppListUpdatedNotification", 51, 53, "app/list/updated"),
    (
        "ExternalAgentConfigImportCompletedNotification",
        52,
        54,
        "externalAgentConfig/import/completed",
    ),
    (
        "ExternalAgentConfigImportProgressNotification",
        53,
        55,
        "externalAgentConfig/import/progress",
    ),
    ("HookCompletedNotification", 54, 56, "hook/completed"),
    ("HookStartedNotification", 55, 57, "hook/started"),
    ("SkillsChangedNotification", 56, 58, "skills/changed"),
)


@dataclass(frozen=True, order=True)
class Diagnostic:
    code: str
    location: str
    message: str


class AuditError(RuntimeError):
    def __init__(self, diagnostics: Sequence[Diagnostic]) -> None:
        self.diagnostics = tuple(diagnostics)
        self.codes = tuple(sorted({row.code for row in diagnostics}))
        super().__init__(
            "; ".join(
                f"{row.code} at {row.location}: {row.message}"
                for row in diagnostics
            )
        )


def _run(repo_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        arguments,
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


def _git_blob(repo_root: Path, revision: str, path: str) -> str:
    return _run(repo_root, "git", "show", f"{revision}:{path}")


def _git_blob_exact(repo_root: Path, revision: str, path: str) -> str:
    result = subprocess.run(
        ("git", "show", f"{revision}:{path}"),
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object-valued JSON: {path}")
    return value


def _render(value: Mapping[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _variant(source: str, alias: str) -> list[str]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(?P<body>.*?)>;",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"unable to find variant {alias}")
    result = [
        re.sub(r"\s+", "", row)
        for row in match.group("body").split(",")
        if row.strip()
    ]
    if not result or any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", row) for row in result):
        raise ValueError(f"{alias} is not a flat named-alternative variant")
    return result


def _key(category: str, domain: str, name: str) -> dict[str, str]:
    return {
        "category": category,
        "domain": domain,
        "discriminator_field": "method" if category != "tagged_union_discriminator" else "type",
        "name": name,
    }


def _identity_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for name, (params, result, kind) in sorted(REQUEST_CONTRACTS.items()):
        rows.append(
            {
                "protocol_surface_key": _key(
                    "client_request", "ClientRequest", name
                ),
                "parameter_type": params,
                "result_type": result,
                "result_kind": kind,
            }
        )
    for name in NOTIFICATIONS:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "server_notification", "ServerNotification", name
                ),
                "parameter_type": "",
                "result_type": "",
                "result_kind": "NotApplicable",
            }
        )
    for name in PLUGIN_SOURCE_ORDER:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "tagged_union_discriminator", "PluginSource", name
                ),
                "parameter_type": "",
                "result_type": "",
                "result_kind": "NotApplicable",
            }
        )
    return sorted(
        rows,
        key=lambda row: (
            row["protocol_surface_key"]["category"],
            row["protocol_surface_key"]["domain"],
            row["protocol_surface_key"]["name"],
        ),
    )


def _object_policy_counts(closure: Mapping[str, Any]) -> tuple[int, int, int]:
    policies = closure.get("object_policies", [])
    open_count = sum(
        row.get("additional_properties") == "allowed_by_default"
        for row in policies
    )
    closed_count = sum(
        row.get("additional_properties") == "False"
        for row in policies
    )
    schema_count = sum(
        row.get("additional_properties") == "schema"
        for row in policies
    )
    return open_count, closed_count, schema_count


def build_reports(arguments: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    repo_root = arguments.repo_root.resolve()
    base_tree = _run(repo_root, "git", "show", "-s", "--format=%T", EXPECTED_BASE_SHA)
    events_source = _git_blob(
        repo_root,
        EXPECTED_BASE_SHA,
        "src/ai/openai/codex/typed/Events.h",
    )
    server_requests_source = _git_blob(
        repo_root,
        EXPECTED_BASE_SHA,
        "src/ai/openai/codex/typed/ServerRequests.h",
    )
    predecessor_plan = _load(arguments.predecessor_plan)
    frozen_variants = predecessor_plan["public_api_plan"][
        "variant_and_layout_plan"
    ]["current"]
    expected_canonical = [row["type"] for row in frozen_variants["CanonicalServerNotification"]]
    expected_events = [row["type"] for row in frozen_variants["Event"]]
    actual_canonical = _variant(events_source, "CanonicalServerNotification")
    actual_events = _variant(events_source, "Event")

    native_closure = _load(arguments.native_closure)
    user_batch = predecessor_plan["implementation_batches"][0]
    closure = user_batch["schema_closure"]
    reachable = {
        (item["namespace"], item["name"])
        for item in closure["reachable_definitions"]
    }
    object_policies = [
        row
        for row in native_closure["object_policies"]
        if (row["definition"]["namespace"], row["definition"]["name"])
        in reachable
    ]
    schema_path_rows = [
        row
        for row in native_closure["schema_paths"]
        if (row["definition"]["namespace"], row["definition"]["name"])
        in reachable
    ]
    open_count, closed_count, schema_count = _object_policy_counts(
        {"object_policies": object_policies}
    )
    closure_counts = dict(closure["counts"])
    closure_counts.update(
        {
            "open_objects": open_count,
            "closed_objects": closed_count,
            "schema_valued_additional_properties": schema_count,
        }
    )

    extraction_manifest = _load(arguments.extraction_manifest)
    source = extraction_manifest["source"]
    snodec_commit = source["commit"]
    snodec_tree = source["tree"]

    cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    codex_cmake = _git_blob_exact(
        repo_root,
        EXPECTED_BASE_SHA,
        "src/ai/openai/codex/CMakeLists.txt",
    )
    soversion_match = re.search(
        r"set\(AISUITE_CODEX_SOVERSION\s+(\d+)\)", cmake
    )
    project_match = re.search(
        r"project\(\s*AISuite\s+VERSION\s+([0-9.]+)", cmake, re.DOTALL
    )
    if soversion_match is None or project_match is None:
        raise ValueError("unable to parse project version or Codex SOVERSION")

    start = {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated PR-A predecessor and package-boundary evidence; do not edit."
        ),
        "actual_base": {
            "sha": EXPECTED_BASE_SHA,
            "tree": base_tree,
        },
        "project": {
            "version": project_match.group(1),
            "codex_soversion": int(soversion_match.group(1)),
        },
        "protocol_authority": {
            "codex_version": CODEX_VERSION,
            "upstream_tag": UPSTREAM_TAG,
            "upstream_source_commit": UPSTREAM_SOURCE_COMMIT,
        },
        "extraction_source": {
            "repository": source["repository"],
            "commit": snodec_commit,
            "tree": snodec_tree,
        },
        "registry_start": {
            "global": {
                "Complete": 280,
                "Partial": 4,
                "NotImplemented": 55,
                "NotApplicable": 48,
                "Total": 387,
            },
            "native_a1_4": {
                "Complete": 0,
                "Partial": 1,
                "NotImplemented": 55,
                "Total": 56,
            },
            "partials": (
                "error",
                "initialize",
                "initialized",
                "item/tool/requestUserInput",
            ),
        },
        "predecessor_variants": {
            "CanonicalServerNotification": [
                {"index": index, "type": name}
                for index, name in enumerate(actual_canonical)
            ],
            "Event": [
                {"index": index, "type": name}
                for index, name in enumerate(actual_events)
            ],
            "expected_sizes": {
                "CanonicalServerNotification": 51,
                "Event": 53,
            },
            "matches_frozen_mapping": (
                actual_canonical == expected_canonical
                and actual_events == expected_events
            ),
        },
        "predecessor_source_hashes": {
            "src/ai/openai/codex/typed/Events.h": _sha256(events_source),
            "src/ai/openai/codex/typed/ServerRequests.h": _sha256(
                server_requests_source
            ),
        },
        "package_boundary": {
            "find_package": (
                "find_package(snodec CONFIG REQUIRED COMPONENTS core net-un-stream-legacy)"
                in cmake
            ),
            "installed_components": ("core", "net-un-stream-legacy"),
            "forbidden_source_relative_dependency": False,
            "genuine_installed_consumer_required": True,
        },
        "public_install_file": "src/ai/openai/codex/CMakeLists.txt",
        "public_install_file_sha256": _sha256(codex_cmake),
    }

    plan = {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated frozen six-commit plan for Codex A1.4 user integrations; do not edit."
        ),
        "base": {"sha": EXPECTED_BASE_SHA, "tree": EXPECTED_BASE_TREE},
        "scope": {
            "identities": _identity_rows(),
            "identity_count": 33,
            "taxonomy": {
                "client_requests": 23,
                "server_notifications": 6,
                "server_requests": 0,
                "tagged_union_alternatives": 4,
            },
            "result_contracts": {"Concrete": 20, "Unit": 3},
            "excluded_slices": ("PR-B", "PR-C", "InventoryOnly", "A1.0"),
        },
        "schema_closure": {
            "counts": closure_counts,
            "seed_definitions": closure["seed_definitions"],
            "reachable_definitions": closure["reachable_definitions"],
            "schema_paths": schema_path_rows,
            "closed_object_negative_case_policy": (
                "The PR-A closure has no closed object. Closed-object rejection "
                "remains covered by an inherited stable closed-object codec."
            ),
            "opaque_json_case_policy": (
                "The PR-A closure has no deliberately opaque JSON path. Open-object "
                "future-field preservation is covered instead."
            ),
        },
        "stages": [dict(stage) for stage in STAGES],
        "six_commit_subjects": (
            "Freeze Codex A1.4 user-integration implementation batches",
            "Complete Codex apps, external agents, and feedback",
            "Complete Codex hooks, marketplace, and skills",
            "Complete Codex plugin operations without source unions",
            "Complete Codex plugin source and catalog operations",
            "Close and verify Codex A1.4 user integrations",
        ),
        "public_api": {
            facade: {
                "accessor": row["accessor"],
                "header": row["header"],
                "methods": list(row["methods"]),
            }
            for facade, row in PUBLIC_API.items()
        },
        "codec_units": CODEC_UNITS,
        "descriptor_files": (
            "detail/ClientOperationCodecDescriptors.inc",
            "detail/ServerNotificationCodecDescriptors.inc",
            "detail/IntegrationsAndLongTailUnionCodecDescriptors.inc",
        ),
        "fixtures": {
            "required_roots": 52,
            "authoritative_root": (
                "tools/codex/app-server-fixtures/0.144.6/cases/user-integrations"
            ),
        },
        "plugin_source": {
            "registry_order": PLUGIN_SOURCE_ORDER,
            "public_variant_order": (
                "GitPluginSource",
                "LocalPluginSource",
                "NpmPluginSource",
                "RemotePluginSource",
                "UnknownPluginSource",
            ),
            "reaching_request_roots": (
                "plugin/installed",
                "plugin/list",
                "plugin/read",
                "plugin/share/list",
            ),
            "commit_4_reaches_plugin_source": False,
            "npm_build_or_runtime_dependency": False,
        },
        "notification_append": {
            "final_sizes": {
                "CanonicalServerNotification": 57,
                "Event": 59,
            },
            "mapping": [
                {
                    "type": type_name,
                    "canonical_index": canonical_index,
                    "event_index": event_index,
                    "method": method,
                }
                for type_name, canonical_index, event_index, method in APPENDS
            ],
        },
        "architecture": {
            "raw_protocol_instances": 1,
            "pending_operation_maps": 1,
            "notification_dispatchers": 1,
            "observer_mechanisms": 1,
            "backend_state_expansion": False,
            "frontend_protocol_expansion": False,
        },
        "integrity": {
            "codex_soversion": EXPECTED_SOVERSION,
            "snodec_source_commit": EXPECTED_SNODEC_SOURCE,
            "snodec_source_tree": EXPECTED_SNODEC_TREE,
            "codex_pin": CODEX_VERSION,
            "npm_dependency": False,
        },
    }
    # Normalize tuples to the exact JSON model used by checked evidence and
    # mutation tests.
    return (
        json.loads(json.dumps(start, ensure_ascii=False)),
        json.loads(json.dumps(plan, ensure_ascii=False)),
    )


def report_diagnostics(
    start: Mapping[str, Any], plan: Mapping[str, Any]
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []

    def require(
        condition: bool, code: str, location: str, message: str
    ) -> None:
        if not condition:
            diagnostics.append(Diagnostic(code, location, message))

    require(
        start.get("actual_base")
        == {"sha": EXPECTED_BASE_SHA, "tree": EXPECTED_BASE_TREE},
        "UserIntegrationPredecessorEvidenceDrift",
        "$.actual_base",
        "actual base SHA/tree changed",
    )
    authority = start.get("protocol_authority", {})
    require(
        authority
        == {
            "codex_version": CODEX_VERSION,
            "upstream_tag": UPSTREAM_TAG,
            "upstream_source_commit": UPSTREAM_SOURCE_COMMIT,
        },
        "UserIntegrationPredecessorEvidenceDrift",
        "$.protocol_authority",
        "Codex authority changed",
    )
    require(
        start.get("project", {}).get("codex_soversion") == EXPECTED_SOVERSION
        and plan.get("integrity", {}).get("codex_soversion")
        == EXPECTED_SOVERSION,
        "UserIntegrationSOVERSIONDrift",
        "$.project.codex_soversion",
        "Codex SOVERSION changed",
    )
    extraction = start.get("extraction_source", {})
    require(
        extraction.get("commit") == EXPECTED_SNODEC_SOURCE
        and extraction.get("tree") == EXPECTED_SNODEC_TREE,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.extraction_source",
        "SNode.C extraction provenance changed",
    )
    variants = start.get("predecessor_variants", {})
    canonical = variants.get("CanonicalServerNotification", [])
    events = variants.get("Event", [])
    canonical_types = [
        row.get("type") for row in canonical if isinstance(row, Mapping)
    ]
    event_types = [
        row.get("type") for row in events if isinstance(row, Mapping)
    ]
    require(
        len(canonical) == 51 and len(events) == 53,
        "UserIntegrationNotificationBaseIndexMismatch",
        "$.predecessor_variants",
        "predecessor variant size changed",
    )
    require(
        variants.get("matches_frozen_mapping") is True
        and all(
            row.get("index") == index
            for index, row in enumerate(canonical)
        )
        and all(
            row.get("index") == index for index, row in enumerate(events)
        )
        and _sha256(
            json.dumps(canonical_types, separators=(",", ":"))
        )
        == EXPECTED_CANONICAL_VARIANT_HASH
        and _sha256(json.dumps(event_types, separators=(",", ":")))
        == EXPECTED_EVENT_VARIANT_HASH,
        "UserIntegrationNotificationBaseIndexMismatch",
        "$.predecessor_variants",
        "predecessor variant type/index mapping changed",
    )
    package = start.get("package_boundary", {})
    require(
        package.get("find_package") is True,
        "UserIntegrationPackageBoundaryMismatch",
        "$.package_boundary.find_package",
        "installed config-mode SNode.C lookup is missing",
    )
    require(
        package.get("forbidden_source_relative_dependency") is False,
        "UserIntegrationCrossRepoDependencyMismatch",
        "$.package_boundary.forbidden_source_relative_dependency",
        "source-relative SNode.C dependency detected",
    )

    scope = plan.get("scope", {})
    identities = scope.get("identities", [])
    expected_rows = _identity_rows()
    require(
        identities == expected_rows,
        "UserIntegrationIdentitySetMismatch",
        "$.scope.identities",
        "exact PR-A identity set changed",
    )
    if identities != expected_rows:
        expected_keys = {
            json.dumps(row["protocol_surface_key"], sort_keys=True)
            for row in expected_rows
        }
        actual_keys = {
            json.dumps(row.get("protocol_surface_key", {}), sort_keys=True)
            for row in identities
            if isinstance(row, Mapping)
        }
        require(
            not (actual_keys - expected_keys),
            "UserIntegrationScopeLeak",
            "$.scope.identities",
            "PR-B, PR-C, InventoryOnly, or inherited identity leaked into PR A",
        )
    require(
        scope.get("identity_count") == 33
        and scope.get("taxonomy")
        == {
            "client_requests": 23,
            "server_notifications": 6,
            "server_requests": 0,
            "tagged_union_alternatives": 4,
        },
        "UserIntegrationIdentitySetMismatch",
        "$.scope",
        "identity denominator or taxonomy changed",
    )
    require(
        scope.get("result_contracts") == {"Concrete": 20, "Unit": 3},
        "UserIntegrationResultContractMismatch",
        "$.scope.result_contracts",
        "result-kind split changed",
    )
    require(
        all(
            row["parameter_type"] == expected["parameter_type"]
            and row["result_type"] == expected["result_type"]
            and row["result_kind"] == expected["result_kind"]
            for row, expected in zip(identities, expected_rows)
        )
        if len(identities) == len(expected_rows)
        else False,
        "UserIntegrationResultContractMismatch",
        "$.scope.identities",
        "request/result association changed",
    )

    closure_counts = plan.get("schema_closure", {}).get("counts", {})
    require(
        closure_counts == EXPECTED_CLOSURE,
        "UserIntegrationSchemaClosureMismatch",
        "$.schema_closure.counts",
        "52/118/411 schema closure or path taxonomy changed",
    )
    require(
        len(plan.get("schema_closure", {}).get("schema_paths", [])) == 411,
        "UserIntegrationSchemaClosureMismatch",
        "$.schema_closure.schema_paths",
        "required schema path coverage changed",
    )

    stages = plan.get("stages", [])
    expected_stages = json.loads(
        json.dumps([dict(stage) for stage in STAGES], ensure_ascii=False)
    )
    require(
        stages == expected_stages,
        "UserIntegrationStageArithmeticMismatch",
        "$.stages",
        "staged ownership or arithmetic changed",
    )
    api = plan.get("public_api", {})
    expected_api = {
        facade: {
            "accessor": row["accessor"],
            "header": row["header"],
            "methods": list(row["methods"]),
        }
        for facade, row in PUBLIC_API.items()
    }
    require(
        api == expected_api,
        "UserIntegrationDescriptorMismatch",
        "$.public_api",
        "facade accessor, header, method, or signature ownership changed",
    )
    require(
        tuple(plan.get("codec_units", ())) == CODEC_UNITS
        and len(plan.get("descriptor_files", ())) == 3,
        "UserIntegrationDescriptorMismatch",
        "$.codec_units",
        "codec or descriptor ownership changed",
    )
    require(
        plan.get("fixtures", {}).get("required_roots") == 52,
        "UserIntegrationFixtureMismatch",
        "$.fixtures",
        "fixture-root ownership changed",
    )

    plugin = plan.get("plugin_source", {})
    require(
        tuple(plugin.get("registry_order", ())) == PLUGIN_SOURCE_ORDER
        and tuple(plugin.get("public_variant_order", ()))
        == (
            "GitPluginSource",
            "LocalPluginSource",
            "NpmPluginSource",
            "RemotePluginSource",
            "UnknownPluginSource",
        ),
        "UserIntegrationPluginSourceOrderMismatch",
        "$.plugin_source",
        "PluginSource known/unknown alternative order changed",
    )
    require(
        plugin.get("commit_4_reaches_plugin_source") is False,
        "UserIntegrationPromotionStageMismatch",
        "$.plugin_source.commit_4_reaches_plugin_source",
        "PluginSource leaked into the non-source plugin stage",
    )
    require(
        plugin.get("npm_build_or_runtime_dependency") is False
        and plan.get("integrity", {}).get("npm_dependency") is False,
        "UserIntegrationPluginSourceDependencyLeak",
        "$.plugin_source.npm_build_or_runtime_dependency",
        "npm became an AISuite dependency",
    )

    append = plan.get("notification_append", {})
    require(
        append.get("final_sizes")
        == {"CanonicalServerNotification": 57, "Event": 59}
        and append.get("mapping")
        == [
            {
                "type": type_name,
                "canonical_index": canonical_index,
                "event_index": event_index,
                "method": method,
            }
            for type_name, canonical_index, event_index, method in APPENDS
        ],
        "UserIntegrationNotificationAppendIndexMismatch",
        "$.notification_append",
        "final append size/type/index mapping changed",
    )
    architecture = plan.get("architecture", {})
    require(
        architecture
        == {
            "raw_protocol_instances": 1,
            "pending_operation_maps": 1,
            "notification_dispatchers": 1,
            "observer_mechanisms": 1,
            "backend_state_expansion": False,
            "frontend_protocol_expansion": False,
        },
        "UserIntegrationFalseComplete",
        "$.architecture",
        "one-RawProtocol or no-product-expansion invariant changed",
    )
    return sorted(set(diagnostics))


def validate_reports(
    start: Mapping[str, Any], plan: Mapping[str, Any]
) -> None:
    diagnostics = report_diagnostics(start, plan)
    if diagnostics:
        raise AuditError(diagnostics)


def write_or_check(
    path: Path, value: Mapping[str, Any], *, check: bool
) -> None:
    rendered = _render(value)
    if check:
        if not path.is_file() or path.read_text(encoding="utf-8") != rendered:
            raise AuditError(
                (
                    Diagnostic(
                        "UserIntegrationPredecessorEvidenceDrift",
                        str(path),
                        "checked generated evidence is stale",
                    ),
                )
            )
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(rendered, encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[2]
    evidence = repo / "tools/codex/app-server-evidence/0.144.6"
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("generate", "check"))
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument(
        "--predecessor-plan",
        type=Path,
        default=evidence / "a1-4-implementation-plan.json",
    )
    result.add_argument(
        "--native-closure",
        type=Path,
        default=evidence / "a1-4-type-closure.json",
    )
    result.add_argument(
        "--extraction-manifest",
        type=Path,
        default=repo / "docs/extraction/source-manifest.json",
    )
    result.add_argument(
        "--start-state",
        type=Path,
        default=evidence / "a1-4-user-integrations-start-state.json",
    )
    result.add_argument(
        "--batch-plan",
        type=Path,
        default=evidence / "a1-4-user-integrations-batch-plan.json",
    )
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        start, plan = build_reports(arguments)
        validate_reports(start, plan)
        check = arguments.command == "check"
        write_or_check(arguments.start_state, start, check=check)
        write_or_check(arguments.batch_plan, plan, check=check)
    except (AuditError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
