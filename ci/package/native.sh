#!/bin/sh
# Dependency installation belongs to trusted preparation.
set -eu
: "${SOURCE_DIR:?}" "${TARGET_JSON:?}" "${OUTPUT_DIR:?}"
format=$(python3 -c 'import json,os; print(json.load(open(os.environ["TARGET_JSON"]))["build"]["format"])')
if [ "$format" = deb ]; then
    libdir="lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
else
    libdir=lib64
    if [ -f /opt/rh/gcc-toolset-14/enable ]; then
        . /opt/rh/gcc-toolset-14/enable
    fi
fi
build=/work/build-product
cmake -S "$SOURCE_DIR" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR="$libdir" -DAISUITE_BUILD_CODEX_TESTS=ON
cmake --build "$build" --parallel "$(nproc)"
ctest --test-dir "$build" --output-on-failure -LE 'real-app-server|end-to-end|live' \
    --output-junit core-ci-tests.xml
DESTDIR=/work/installed cmake --install "$build"
export CMAKE_PREFIX_PATH="/work/installed/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="/work/installed/usr/$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake -S "$SOURCE_DIR/tests/consumer" -B /work/build-consumer -G Ninja
cmake --build /work/build-consumer --parallel "$(nproc)"
ctest --test-dir /work/build-consumer --output-on-failure
python3 /tool/native_container.py --package "$build"
