# P3 Task-A F3 production-coverage audit

This audit maps the three broad tests that Commit 1 retargeted to the frozen
legacy server oracle against the cut-over production adapter and permanent
P2/P3 cores. Its four missing production regressions were externally approved
and are implemented in Commit 2.

Classifications are: **A**, legacy-oracle-only reference behavior; **B**,
continuing production behavior already covered elsewhere; **C**, continuing
production behavior currently missing; and **D**, obsolete old-implementation
authority replaced by a named permanent core test.

## `CodexFrontendProjectionTest`

| ID | Original behavior/assertion group | Class | Current production replacement executable/function | Missing? | Proposed minimal action |
|---|---|---|---|---|---|
| P1 | Old canonicalizer secret/raw/truncation bookkeeping counters | A | None; frozen JSON-projection implementation detail | No | None |
| P2 | Safe detail retains accounting while rejecting credentials/raw envelopes and bounding UTF-8 | D | `CodexFrontendTypedModelTest::testStrongIdentitiesAndSafeDetail`; `CodexFrontendServerCoreSynchronizationTest::testBackendProjection`; `CodexFrontendTypedSnapshotTest::testExpandedItemDetailWireBounds` | No | None |
| P3 | Sensitive command output is scope-filtered identically for Snapshot/live/replay | D | `CodexFrontendProjectionSecurityTest::testItemInformationCeilings`; `CodexFrontendServerDifferentialTest::testScopeAndCapabilityCorpus` | No | None |
| P4 | Snapshot local/remote ceilings, safe pending data, sequence/paging/omission metadata, Observe requirement | D | `CodexFrontendServerCoreSynchronizationTest::testBackendProjection`; `::testIndependentSnapshotCapabilities`; `CodexFrontendProjectionSecurityTest::testItemInformationCeilings`; `CodexFrontendServerDifferentialTest::testScopeAndCapabilityCorpus` | No | None |
| P5 | Old recursive visit-budget/JSON-Pointer rule-engine mechanics | A | None; typed projection has no generic visitor/rule engine | No | None |
| P6 | Identity-less/unsupported items are omitted with truncation, never fabricated IDs | D | `CodexFrontendServerCoreSynchronizationTest::testUnknownBackendItemContainment`; `::testUnknownBackendItemWireContainment` | No | None |
| P7 | Complete backend/thread/pending capability independence | D | `CodexFrontendTypedSnapshotTest::testIndependentRepresentationSelection`; `CodexFrontendServerCoreSynchronizationTest::testIndependentSnapshotCapabilities` | No | None |
| P8 | One canonical occurrence owns one sequence across representation/live/replay | D | `CodexFrontendTypedOccurrenceTest::testGeneratedMultiFamilyGroup`; `::testServerCompatibilitySelection`; `CodexFrontendServerCoreSynchronizationTest::testSynchronization`; `::testTypedBatching` | No | None |
| P9 | Empty/representative backend snapshots become schema-valid typed snapshots under both security ceilings | D | `CodexFrontendServerCoreSynchronizationTest::testBackendProjection`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| P10 | All 68 notification mappings preserve ordered families, sequence and one compatibility form | D | `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus`; `CodexFrontendContractMetadataTest::testCompatibilityProjection` | No | None |
| P11 | Thread-list update has one compact exact representation and fabricates no empty-page thread | D | `CodexFrontendServerCoreSynchronizationTest::testExactBackendOccurrenceIdentity`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| P12 | All 18 item alternatives retain exact identities/parents/safe fields and compatibility splits | D | `CodexFrontendTypedModelTest::testTypedVariantsAndInvalidation`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus`; `CodexFrontendContractMetadataTest::testCompatibilityProjection` | No | None |
| P13 | Backend item discriminators normalize to stable wire enums; no raw authority crosses boundary | D | `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus`; `CodexFrontendServerCoreSynchronizationTest::testExactBackendOccurrenceIdentity`; `CodexFrontendTypedOccurrenceTest::testOccurrenceIdentityValidation` | No | None |
| P14 | Exact thread/turn/item tuples reject duplicates, inconsistent parents and substitutions | D | `CodexFrontendServerCoreSynchronizationTest::testExactBackendOccurrenceIdentity` | No | None |
| P15 | Unknown item kinds force bounded Snapshot containment without losing known neighbors | D | `CodexFrontendServerCoreSynchronizationTest::testUnknownBackendItemContainment`; `::testUnknownBackendItemWireContainment` | No | None |
| P16 | Ten pending-request kinds retain exact safe presentation/location/kind/input and negotiated form | D | `CodexFrontendContractMetadataTest::testPendingRequestProjection`; `CodexFrontendServerCoreSynchronizationTest::testUnknownPendingRequestCompatibility`; `::testPendingUserInputPresentationUtf8Containment`; `CodexFrontendTypedOccurrenceTest::testLegacyPendingPresentationAndDiagnostics` | No | None |
| P17 | All 26 expanded event families are schema-valid, sequence-stable and scope-controlled | D | `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus`; `CodexFrontendProjectionSecurityTest::testGeneratedOccurrenceAuthority` | No | None |
| P18 | Known notifications choose one negotiated form; unknown safe notifications become bounded extensions | D | `CodexFrontendServerCoreSynchronizationTest::testSynchronization`; `::testBackendProjection`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| P19 | Command/file output filtering is capability-independent and live/replay consistent | D | `CodexFrontendProjectionSecurityTest::testItemInformationCeilings`; `CodexFrontendServerDifferentialTest::testScopeAndCapabilityCorpus` | No | None |
| P20 | Old greater-than-128 JSON projection-rule implementation limit | A | None; typed projection has no accumulated rule budget | No | None |
| P21 | Discriminator-specific output ceilings remain exact at volume and fail closed | D | `CodexFrontendProjectionSecurityTest::testItemInformationCeilings`; `CodexFrontendServerCoreSynchronizationTest::testUnknownBackendItemContainment` | No | None |
| P22 | Nested legacy thread/turn/item security ceiling matches Snapshot/live/replay/expanded | B | `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus`; `::testScopeAndCapabilityCorpus` | No | None |
| P23 | Exact domain identities; conflicting/redacted/truncated identities fail closed; 25-row page keeps per-ID content | D | `CodexFrontendServerCoreSynchronizationTest::testExactBackendOccurrenceIdentity` | No | None |
| P24 | Runtime metadata consumes 68 notifications, 18 items, 10 pending kinds, 26 event families and compatibility splits | D | `CodexFrontendContractMetadataTest::testCompatibilityProjection`; `::testPendingRequestProjection`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |

Result: this surface has no C row. Old generic JSON-visitor bookkeeping is
oracle-only; continuing projection/security semantics have named typed/core
coverage.

## `CodexFrontendAuthorizationMetadataTest`

| ID | Original behavior/assertion group | Class | Current production replacement executable/function | Missing? | Proposed minimal action |
|---|---|---|---|---|---|
| A1 | All 105 methods have complete implementation/schema/readiness/registry/backend/legacy metadata and exact counts | D | `CodexFrontendContractMetadataTest::testCounts`; `::testParameterAndAvailabilityRoundTrips`; `CodexFrontendRuntimeBackendMappingTest::testCompleteTable`; `CodexFrontendProtocolGeneratorTest.py::main` | No | None |
| A2 | Default-remote exclusion decomposes into 22 privileged, 12 reverse, 3 lifecycle | D | `CodexFrontendProtocolGeneratorTest.py::main`; `CodexProtocolSurfaceCoverageGuardTest::main` | No | None |
| A3 | Independent security-owner derivation and registry/native counts | D | `CodexFrontendProtocolGeneratorTest.py::main`; `CodexProtocolSurfaceCoverageGuardTest::main` | No | None |
| A4 | Exact 15 default-disabled and 15 legacy-compatible methods | D | `CodexFrontendProtocolGeneratorTest.py::main`; `CodexFrontendContractMetadataTest::testParameterAndAvailabilityRoundTrips`; `::testExistingProtocolAliases` | No | None |
| A5 | Thirteen static, one conditional multi-transport and four product capabilities | D | `CodexFrontendProtocolGeneratorTest.py::main`; `CodexFrontendServerDifferentialTest::testScopeAndCapabilityCorpus` | No | None |
| A6 | SDK on/off × one/two-family runtime capability truth and per-handshake immutability | C | `CodexFrontendServerCoreLifecycleTest::testCapabilityTruthAndHandshakeFreeze`; `CodexFrontendPublicServiceAdapterTest::testPublicAdapter` | No | Implemented with four actual ServerCore configurations, old-Welcome immutability, and public build-truth conversion |
| A7 | `account.read` omitted/false refresh is observer; true refresh requires stronger policy | B | `CodexFrontendProtocolGeneratorTest.py::main`; `CodexFrontendRuntimeActivationTest::testFalseAccountReadRetry`; `::testDeniedTokenRefreshes`; `::testPermittedTokenRefresh` | No | None |

## `CodexFrontendServiceTest`

| ID | Original behavior/assertion group | Class | Current production replacement executable/function | Missing? | Proposed minimal action |
|---|---|---|---|---|---|
| S1 | Public `EventCoalescer`/`UpdateBatchBuilder` compatibility and bounds | B | Current `CodexFrontendServiceTest::testCoalescingAndBounds` still uses unrenamed production utilities; `CodexFrontendProtocolV1Test::main` | No | None |
| S2 | Malformed expanded producer event is rejected before transport | D | `CodexFrontendProtocolV1Test::testProducerExpandedEventValidation`; `CodexFrontendServerCoreSynchronizationTest::testTypedBatching` | No | None |
| S3 | Verified Unix trust, spoof/wrong/missing identity rejection and insecure override | B | `CodexFrontendServerCoreSecurityTest::testLocalTrust`; `CodexFrontendPublicServiceAdapterTest::testPublicAdapter` | No | None |
| S4 | Unauthenticated capacity; auth-before-backend-session; success creates one command session | B | `CodexFrontendPublicServiceAdapterTest::testPublicAdapter`; `::testBackendSessionAdmissionFailure`; `CodexFrontendServerCoreLifecycleTest::testLifecycle` | No | None |
| S5 | IPv4 failed-auth key strips ports, expires windows and bounds peers | B | `CodexFrontendServerCoreSecurityTest::testAuthenticationBudgetAndSanitization` | No | None |
| S6 | Raw/bracketed IPv6 share a key; RFCOMM/TLS channels share Bluetooth address key | C | `CodexFrontendServerCoreSecurityTest::testAuthenticationBudgetAndSanitization` | No | Implemented as focused IPv6 and RFCOMM key-pair checks |
| S7 | Capability totals, multi-transport transitions and existing-Welcome immutability | C | `CodexFrontendServerCoreLifecycleTest::testCapabilityTruthAndHandshakeFreeze` | No | Implemented by the single lifecycle regression shared with A6 |
| S8 | Timeouts/capacities/rate/frame/pre-auth concealment/bounded failed peers | B | `CodexFrontendServerCoreLifecycleTest::testLifecycle`; `CodexFrontendServerCoreSecurityTest::testAuthenticationBudgetAndSanitization`; `::testRateScopesAndController`; `CodexFrontendServerCoreCommandTest::testOutstandingCapacity`; `CodexFrontendServerCoreBackpressureTest::testQueueAndTransportBackpressure` | No | None |
| S9 | Hello/Welcome/Snapshot/Sync order, distinct sessions/counts/public queries/callback conversion | B | `CodexFrontendPublicServiceAdapterTest::testPublicAdapter`; `CodexFrontendServerCoreLifecycleTest::testLifecycle`; `CodexFrontendServiceClientIntegrationTest::IntegrationRunner::expectInitialState` | No | None |
| S10 | Independent BackendCore session mutation advances frontend journal/replay | B | `CodexFrontendPublicServiceAdapterTest::testExternalBackendTopologyCompatibility` | No | None |
| S11 | Controller acquire/release/conflict/correlation/duplicate ID/observer denial/disconnect | B | `CodexFrontendPublicServiceAdapterTest::testPublicAdapter`; `CodexFrontendServerCoreSynchronizationTest::testSessionAndControllerJournal`; `CodexFrontendServerCoreSecurityTest::testRateScopesAndController`; `CodexFrontendServerCoreCommandTest::testSynchronousCompletionAndStaleGeneration` | No | None |
| S12 | External controller/session topology, replay/Snapshot, echo suppression and close handoff | B | `CodexFrontendPublicServiceAdapterTest::testExternalBackendTopologyCompatibility`; `::testControllerCompletionWaitsForObserverFence`; `::testUnexpectedBackendClosePreservesExternalControllerHandoff` | No | None |
| S13 | Reverse-response readiness, secrecy, bounded mapping/remote errors | D | `CodexFrontendServerCoreCommandTest::testBackendSubmissionErrors`; `CodexFrontendRuntimeSecurityTest::testPrivateMappingFailureRedaction`; `::ProviderSecurityRunner::testRemoteFailureRedaction`; `CodexFrontendProviderResultProjectionTest::testProjectionSafetyAndCorrelation` | No | None |
| S14 | Ordered observers, replay/gap Snapshot/future cursor/pre-Hello isolation | D | `CodexFrontendServerCoreSynchronizationTest::testSynchronization`; `::testTypedBatching`; `::testSessionAndControllerJournal`; `CodexFrontendServerCoreLifecycleTest::testLifecycle` | No | None |
| S15 | Throwing/self-closing/slow callbacks, bounded queues, one close | B | `CodexFrontendServerCoreBackpressureTest::testQueueAndTransportBackpressure`; `::testDeliveryFairness`; `::testNestedFlushCoalescing`; `CodexFrontendServerCoreLifecycleTest::testExceptionBoundaries`; `::testExplicitReentrancy`; `CodexFrontendPublicServiceAdapterTest::testPublicAdapter`; `::testReentrantServiceCloseFromCommandCompletion` | No | None |
| S16 | Method discovery/permitted counts; deployment denial precedes schema/backend | B | `CodexFrontendRuntimeActivationTest::verifyAuthenticationAndCatalog`; `CodexFrontendRuntimeSecurityTest::testDeploymentBeforeSchemaAndConfiguredPolicy`; `CodexFrontendServerCoreSecurityTest::testConditionalMethods` | No | None |
| S17 | Unchanged Snapshot does not advance; dirty/fallback barrier advances once | D | `CodexFrontendServerCoreSynchronizationTest::testSynchronization`; `CodexFrontendServerCoreBackpressureTest::testMixedSnapshotFallbackAdvancesSequence`; `CodexFrontendServiceClientIntegrationTest::IntegrationRunner::verifyForcedList` | No | None |
| S18 | Capacity-only Backend snapshot cannot create service observer feedback loop | C | `CodexFrontendPublicServiceAdapterTest::testCapacitySnapshotFeedbackQuiescence` | No | Implemented as one bounded capacity update followed to scheduler quiescence |
| S19 | Sparse global sequence across hidden scoped occurrence, replay, hidden suffix and Snapshot | C | `CodexFrontendServerCoreSynchronizationTest::testScopeFilteredSparseReplay` | No | Implemented as a four-sequence visible/hidden replay and Snapshot check |
| S20 | Canonical multi-family occurrence is indivisible and sequence-identical | D | `CodexFrontendTypedOccurrenceTest::testGeneratedMultiFamilyGroup`; `CodexFrontendServerCoreSynchronizationTest::testTypedBatching`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| S21 | Exact six legacy provider wrappers and all generated command/result ownership | D | `CodexFrontendRuntimeBackendMappingTest::testCompleteTable`; `::testExactValues`; `::testTypedLegacyMethodBoundary`; `CodexFrontendProviderResultProjectionTest::testExactResultInventoryAndCorpus`; `::testLegacySixBoundary`; `CodexFrontendServerCoreCommandTest::testGeneratedDispatchAndCorrelation`; `CodexFrontendRuntimeActivationTest::testAdditiveProviderResult` | No | None |
| S22 | Unknown safe/oversized extensions are redacted/bounded/retained without secrets | D | `CodexFrontendServerCoreSynchronizationTest::testBackendProjection`; `CodexFrontendTypedOccurrenceTest::testContainedExtension`; `CodexFrontendTypedSnapshotTest::testLegacyShapeAndRoundTrip`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| S23 | End-to-end BackendCore → public service → client live/replay/Snapshot/command | B | `CodexFrontendServiceClientIntegrationTest::IntegrationRunner::verifyCompactList`; `::verifyForcedList`; `::verifyTurnLifecycle`; `::verifyLifecycleReplayAndBeginSecondCommand` | No | None |
| S24 | High-volume coalescing, bounded queues, fair delivery, terminal order | D | `CodexFrontendServerCoreSynchronizationTest::testInlineObserverBatchCoalescing`; `CodexFrontendServerCoreBackpressureTest::testQueueAndTransportBackpressure`; `::testDeliveryFairness`; `::testNestedFlushCoalescing` | No | None |
| S25 | User truncation, metadata-only forms, unknown containment, no fabricated lifecycle | D | `CodexFrontendServerCoreSynchronizationTest::testBackendProjection`; `::testUnknownBackendItemContainment`; `CodexFrontendServerDifferentialTest::testAuthorityProjectionCorpus` | No | None |
| S26 | Capacity eviction creates one authoritative all-client Snapshot and invalidates replay | D | `CodexFrontendServerCoreBackpressureTest::testPublishResultDeliveryContract`; `::testMixedSnapshotFallbackAdvancesSequence`; `CodexFrontendServiceClientIntegrationTest::IntegrationRunner::verifyForcedList` | No | None |
| S27 | Pending presentation/resolution/invalidation is exact and single-authority | D | `CodexFrontendTypedOccurrenceTest::testPendingOrderingAndAuthorityMigration`; `::testLegacyPendingPresentationAndDiagnostics`; `CodexFrontendServerCoreSynchronizationTest::testUnknownPendingRequestCompatibility`; `::testPendingUserInputPresentationUtf8Containment` | No | None |
| S28 | Event-loop watchdog/runner completion bookkeeping | A | None; harness-only | No | None |

## Approved production coverage

The four approved gaps are now covered without restoring the three broad legacy
tests to the production target: capability truth/handshake freeze; IPv6/RFCOMM
admission-key normalization; sparse scope-filtered replay; and capacity-snapshot
feedback quiescence at the public service/bridge border.
