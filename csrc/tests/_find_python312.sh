# Shared Python 3.12 locator for the test harness.
# Sourced by run_*.sh scripts. Sets $PYTHON or exits 2.
#
# Order:
#   1. $PYTHON env override (must be exec)
#   2. /opt/homebrew/bin/python3.12        (macOS Homebrew)
#   3. /usr/local/bin/python3.12           (Intel Homebrew / generic)
#   4. /usr/bin/python3.12                 (Linux distro packaged)
#   5. python3.12 in $PATH                 (Linux CI, deadsnakes PPA)
#   6. python.exe / python3.exe in $PATH, version-checked == 3.12.x
#      (Windows actions/setup-python installs as `python.exe` only)
#
# Strict: refuses to silently fall back to system `python3` (would pick up
# 3.9 on macOS and 3.10/3.11 on older distros — those don't import the
# upstream src/ tree cleanly because of StrEnum and similar 3.12+ idioms).

# Try a candidate; accept if it reports a 3.12.x version.
_py_is_312() {
    local p="$1"
    [ -n "$p" ] || return 1
    [ -x "$p" ] || return 1
    "$p" -c 'import sys; sys.exit(0 if sys.version_info[:2]==(3,12) else 1)' >/dev/null 2>&1
}

if [ -z "${PYTHON:-}" ] || ! _py_is_312 "$PYTHON"; then
    PYTHON=""
    for _cand in \
            /opt/homebrew/bin/python3.12 \
            /usr/local/bin/python3.12 \
            /usr/bin/python3.12 \
            "$(command -v python3.12 2>/dev/null || true)" \
            "$(command -v python.exe 2>/dev/null || true)" \
            "$(command -v python3.exe 2>/dev/null || true)" \
            "$(command -v python 2>/dev/null || true)"; do
        if _py_is_312 "$_cand"; then
            PYTHON="$_cand"
            break
        fi
    done
    unset _cand
fi

if [ -z "${PYTHON:-}" ] || ! _py_is_312 "$PYTHON"; then
    echo "ERROR: required interpreter Python 3.12.x not found." >&2
    echo "       Tried: \$PYTHON env, /opt/homebrew/bin, /usr/local/bin, /usr/bin," >&2
    echo "              python3.12 / python.exe / python3.exe / python in PATH." >&2
    echo "       Strict harness will not silently fall back to other Python versions." >&2
    exit 2
fi
unset -f _py_is_312
