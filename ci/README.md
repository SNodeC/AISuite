# Repository-owned package builds

Schema 2 declares the tagged source's existing SNode.C requirement as `>=2.0,<3`. No product requirement, target list or existing family prerequisite list changes. Rocky 9's compiler selection and activation belong to this repository's declaration and hook.

The chain supplies tagged source/submodules in `SOURCE_DIR`, independent CI assets in `CI_DIR`, and frozen dependency selections in `DEPENDENCIES_JSON`. The native hook selects its CPack project configuration through `CI_DIR`; local CPack uses that same configuration. Native metadata records exact resolved versions/revisions. Core-ci validates compatibility ranges while planning. With SNode.C 3 selected, this source remains blocked until a compatible source release is published.
