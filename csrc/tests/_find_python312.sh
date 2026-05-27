# Shared Python 3.12 locator for the test harness.
# Sourced by run_*.sh scripts. Sets $PYTHON or exits 2.
#
# Order:
#   1. $PYTHON env override (must be exec)
#   2. /opt/homebrew/bin/python3.12        (macOS Homebrew)
#   3. /usr/local/bin/python3.12           (Intel Homebrew / generic)
#   4. /usr/bin/python3.12                 (Linux distro packaged)
#   5. python3.12 in $PATH                 (CI runners, deadsnakes PPA, etc.)
#
# Strict: refuses to silently fall back to system `python3` (would pick up
# 3.9 on macOS and 3.10/3.11 on older distros — those don't import the
# upstream src/ tree cleanly because of StrEnum and similar 3.12+ idioms).

if [ -z "${PYTHON:-}" ] || [ ! -x "$PYTHON" ]; then
    for _cand in \
            /opt/homebrew/bin/python3.12 \
            /usr/local/bin/python3.12 \
            /usr/bin/python3.12 \
            "$(command -v python3.12 2>/dev/null || true)"; do
        if [ -n "$_cand" ] && [ -x "$_cand" ]; then
            PYTHON="$_cand"
            break
        fi
    done
    unset _cand
fi

if [ -z "${PYTHON:-}" ] || [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter python3.12 not found." >&2
    echo "       Tried: \$PYTHON env, /opt/homebrew/bin, /usr/local/bin, /usr/bin, PATH." >&2
    echo "       Strict harness will not silently fall back to system python3." >&2
    exit 2
fi
