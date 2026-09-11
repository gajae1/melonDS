#!/bin/bash
set -euo pipefail

if [[ ${MINGW_PREFIX:-} != /ucrt64 ]]; then
    echo "Run this script in an MSYS2 UCRT64 shell." >&2
    exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
python_bin=${PYTHON:-"$MINGW_PREFIX/bin/python.exe"}
if [[ ! -x $python_bin ]]; then
    echo "Set PYTHON to a native Windows Python 3.11+ executable, or install UCRT64 Python." >&2
    exit 1
fi

# No arguments retains the build-directory entry point. Both outputs must be new.
if (( $# == 0 )); then
    set -- "$(cygpath -am .)" "$(cygpath -am dist)" \
        --runtime-manifest "$(cygpath -am dist-manifest.json)"
fi
exec "$python_bin" "$(cygpath -am "$script_dir/deploy-windows.py")" "$@" \
    --msys-prefix "$(cygpath -am "$MINGW_PREFIX")"
