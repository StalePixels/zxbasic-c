# Shared Python locator for the test harness.
# Sourced by run_*.sh scripts. Sets $PYTHON or exits 2.
#
# Upstream src/api/python_version_check.py requires Python >= 3.11.
# This locator enforces that floor and refuses to silently fall back to
# an older system python3 (still 3.9 on macOS Sonoma, 3.10 on older
# distros — both fail to import upstream src/ because of 3.11+ idioms).
#
# Order:
#   1. $PYTHON env override (must be exec AND >= 3.11)
#   2. Versioned candidates python3.13/3.12/3.11 in /opt/homebrew/bin,
#      /usr/local/bin, /usr/bin, then PATH lookup
#   3. Unversioned python3.exe / python.exe / python3 / python from PATH,
#      accepted only if the candidate self-reports >= 3.11

_py_ok() {
    local p="$1"
    [ -n "$p" ] || return 1
    [ -x "$p" ] || return 1
    "$p" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' \
        >/dev/null 2>&1
}

if [ -z "${PYTHON:-}" ] || ! _py_ok "$PYTHON"; then
    PYTHON=""
    _try_set() {
        if [ -z "$PYTHON" ] && _py_ok "$1"; then PYTHON="$1"; fi
    }
    for _ver in 3.13 3.12 3.11; do
        for _dir in /opt/homebrew/bin /usr/local/bin /usr/bin; do
            _try_set "$_dir/python$_ver"
        done
        _try_set "$(command -v "python$_ver" 2>/dev/null || true)"
    done
    _try_set "$(command -v python3.exe 2>/dev/null || true)"
    _try_set "$(command -v python.exe   2>/dev/null || true)"
    _try_set "$(command -v python3      2>/dev/null || true)"
    _try_set "$(command -v python       2>/dev/null || true)"
    unset _ver _dir
    unset -f _try_set
fi

if [ -z "${PYTHON:-}" ] || ! _py_ok "$PYTHON"; then
    echo "ERROR: required Python >= 3.11 not found." >&2
    echo "       Searched: \$PYTHON env override, python3.{13,12,11} in" >&2
    echo "       /opt/homebrew/bin, /usr/local/bin, /usr/bin, PATH, plus" >&2
    echo "       python3 / python (version-checked >= 3.11)." >&2
    echo "       Strict harness will not silently fall back to older Python." >&2
    exit 2
fi
unset -f _py_ok
