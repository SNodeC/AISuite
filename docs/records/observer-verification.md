<!-- snodec:begin page-header -->
<a id="page-overview"></a>
<p>
  <a href="../../README.md#project-overview" title="AISuite repository"><img src="../media/page-banner.svg" alt="AISuite repository" width="100%"></a>
</p>
<!-- snodec:end page-header -->

# Observer Verification

Historical implementation and verification evidence. This is not a current qualification claim or a user setup procedure.

## Implementation record: local verification of this extension

Before extending the production table, the expanded routing suite rejected all 16 new reads as expected. After the extension, all 159 request classifications, 81 notification methods and 11 server-request methods passed. The existing 34 observer permissions and the controller path are retained.

Commands (2026-09-26):

```sh
cmake --build /tmp/aisuite-project-routing.0Ed8Iw --parallel 14
ctest --test-dir /tmp/aisuite-project-routing.0Ed8Iw \
  -E '^CodexRealAppServer_' --output-on-failure --parallel 14
xvfb-run -a env QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /path/to/workspace/drafts/CodexUI/build/Desktop_GCC-Debug \
  -R '^(codexui-client-runtime-dispatch|codexui-shell-integration)$' \
  --output-on-failure --parallel 14
git diff --check
```

Results: full local AISuite build passed with warnings treated as errors; 24/24 selected AISuite checks passed; both existing local CodexUI checks passed. The latter use their own fixtures/mock bridge, not the changed bridge connected to the user's desktop session. Dedicated `CodexRealAppServer_*` transport cases were excluded. Individual new operations were source-reviewed, not executed against a live authenticated app-server. Server availability and capability gates therefore remain separate from the verified routing policy.

The selected AISuite suite took 1.26 seconds before the change and 1.23 seconds after it. This is a suite runtime observation, not a throughput benchmark or UI smoothness proof. The observer lookup remains a bounded, allocation-free scan of static entries (50 rather than 34); controllers and provider notifications do not execute that lookup. No queues, timers, caches or callbacks were added.

Accounting: production +16/-0 lines; tests +243/-35 lines (net +208). No CodexUI files, installed binaries or running user processes were changed.
