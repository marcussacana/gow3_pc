#!/usr/bin/env bash
# Builds gow3_leia.dll (gpu/leia_bridge, the only code that uses the Leia SR SDK) with MSVC and
# puts it next to out/gow3-probe.exe; the SR runtime DLLs come from the Leia SR platform install.
# Needs Visual Studio 2022 (Build Tools) and the SR SDK (include/sr, lib):
#   LEIASR_SDK_ROOT=C:/path/to/sdk bash packaging/windows/build_leia.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${LEIASR_SDK_ROOT:?Set LEIASR_SDK_ROOT to the Leia SR SDK (include/sr, lib)}
cmake=${CMAKE_EXE:-cmake}
"$cmake" -S gpu/leia_bridge -B out/leia-bridge -G "Visual Studio 17 2022" -A x64 \
    -DLEIASR_SDK_ROOT="$(cygpath -m "$sdk")"
"$cmake" --build out/leia-bridge --config Release
cp out/leia-bridge/Release/gow3_leia.dll out/
echo "Leia bridge ready: out/gow3_leia.dll"
