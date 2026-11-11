# AISuite documentation instructions

## Packaging and workspace layout

Persistent builds use `build/codex/<configuration>` and approved additional checkouts use `checkouts/<task>`; both are ignored. Temporary builds use `/tmp/codex/build/AISuite/<task>`. Never install verification packages on the host.

`VERSION` supplies the CMake project version. The three existing compiled libraries are shared runtime files in `aisuite`; headers, linker names, CMake exports and the frontend SDK belong to `aisuite-dev`. Keep `AISuite::` exported target names. `ci/packages.json` owns native target and dependency policy; `ci/package/native.sh` builds, runs disposable tests, compiles/runs `tests/consumer` against the staged installation, and packages through the shared worker interface. Real app-server, end-to-end and live-provider tests are outside the package hook; they require their documented environments.

Formatting and analysis configurations match the shared canonical baseline. Explicit formatting modules discover owned `src/` and `tests/` C++, root/subdirectory CMake files and the frontend SDK build definition; generated protocol, third-party, build and checkout files are excluded. Ordinary builds never format sources.

## Markdown formatting

- Write each prose paragraph on one physical source line. Do not manually wrap prose to a fixed column width; let the renderer wrap to the available width.
- Keep links and inline code within the same source line as the surrounding sentence. Keep each list item's prose paragraph on one source line too.
- Use blank lines between paragraphs and other blocks. Preserve intentional newlines in code blocks, tables and genuinely line-oriented content; do not insert hard breaks into ordinary prose.
- Check README changes for unintended rendered line breaks, particularly around links. A formatter must not reintroduce hard-wrapped prose.
