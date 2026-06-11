#!/usr/bin/env bash
# fetch.sh — capture + stage the released-program corpus.
#
# WHAT IT DOES (per row in manifest.tsv):
#   1. Download archive_url into cache/<id>.<ext>   (skipped if already cached)
#   2. Verify sha256 against the manifest            (DRIFT GUARD — see below)
#   3. Extract / copy into work/<id>/
#   4. Overlay fixups/<id>/ on top of work/<id>/     (our corpus-side fixes)
#
# DRIFT GUARD: the manifest commits a sha256 for every archive. Upstream sites
# (spectrumcomputing, itch, blogspot, ...) can silently re-pack or vanish. If a
# freshly-downloaded archive's hash != the committed hash, fetch.sh STOPS on
# that row and reports it rather than testing against drifted bytes. Once an
# archive is in cache/ with a matching hash, re-runs use the captured copy and
# never touch the network — that captured copy is the real source of truth.
#
# Bytes (cache/, work/) are gitignored. Only manifest.tsv + fixups/ are tracked.
#
# Usage:
#   ./fetch.sh                 # fetch/stage every row
#   ./fetch.sh <id> [<id>...]  # only the named programs
#   FORCE=1 ./fetch.sh <id>    # re-download even if cached (then re-verify)
#
# Requires: curl, shasum, unzip; tar for .tar.gz; unrar OR 7z for .rar.

set -uo pipefail

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
MANIFEST="$SELF_DIR/manifest.tsv"
CACHE="$SELF_DIR/cache"
WORK="$SELF_DIR/work"
FIXUPS="$SELF_DIR/fixups"

[ -f "$MANIFEST" ] || { echo "ERROR: no manifest.tsv at $MANIFEST" >&2; exit 2; }
mkdir -p "$CACHE" "$WORK"

WANT=" $* "   # space-padded id filter; empty => all

sha_of() { shasum -a 256 "$1" | awk '{print $1}'; }

extract() {  # <archive> <type> <destdir>
    local arc="$1" typ="$2" dest="$3"
    rm -rf "$dest"; mkdir -p "$dest"
    case "$typ" in
        zip)    unzip -o -q -d "$dest" "$arc" ;;
        tar.gz|tgz) tar -xzf "$arc" -C "$dest" ;;
        bas)    cp "$arc" "$dest/$(basename "$arc")" ;;
        rar)
            if command -v unar  >/dev/null 2>&1; then unar -q -f -o "$dest" "$arc" >/dev/null
            elif command -v unrar >/dev/null 2>&1; then unrar x -o+ -inul "$arc" "$dest/" >/dev/null
            elif command -v 7z   >/dev/null 2>&1; then 7z x -y -o"$dest" "$arc" >/dev/null
            elif command -v bsdtar >/dev/null 2>&1; then bsdtar -xf "$arc" -C "$dest"   # libarchive groks rar
            else echo "  ! no unar/unrar/7z/bsdtar for .rar — skipping extract"; return 1; fi ;;
        *) echo "  ! unknown archive_type '$typ'"; return 1 ;;
    esac
    # One pass of nested-archive extraction: real releases sometimes ship a zip
    # of zips (per-language / per-game sub-archives). Extract each *.zip found
    # into a sibling dir named after it (sans .zip), so manifest entry paths can
    # point inside, e.g. "ADLunam(EN)_SourceCode/ad11.bas".
    find "$dest" -name '*.zip' 2>/dev/null | while IFS= read -r z; do
        sub="${z%.zip}"; mkdir -p "$sub"; unzip -o -q -d "$sub" "$z" 2>/dev/null || true
    done
    # Recover traversal: some old zips preserve dir modes without the owner-x
    # bit (drw-r--r--), which makes the files unreadable. Normalise.
    chmod -R u+rwX "$dest" 2>/dev/null || true
}

TOTAL=0; OK=0; DRIFT=0; FAIL=0; SKIP=0
while IFS=$'\t' read -r id name origin archive sha atype entry flags notes; do
    case "$id" in ''|'#'*) continue ;; esac
    if [ -n "$*" ] && [ "${WANT/ $id /}" = "$WANT" ]; then continue; fi
    TOTAL=$((TOTAL+1))

    if [ "$archive" = "NONE" ] || [ -z "$archive" ]; then
        echo "SKIP  $id :: no downloadable archive (frontend source-only / dead link)"
        SKIP=$((SKIP+1)); continue
    fi

    ext="$atype"; [ "$atype" = "tar.gz" ] && ext="tar.gz"
    arc="$CACHE/$id.$ext"

    if [ "${FORCE:-0}" = "1" ] || [ ! -s "$arc" ]; then
        echo "GET   $id <- $archive"
        if ! curl -fsSL -m 120 "$archive" -o "$arc"; then
            echo "  ! download failed"; FAIL=$((FAIL+1)); rm -f "$arc"; continue
        fi
    fi

    got=$(sha_of "$arc")
    if [ "$sha" != "NONE" ] && [ -n "$sha" ] && [ "$got" != "$sha" ]; then
        echo "DRIFT $id :: sha256 mismatch!"
        echo "        manifest: $sha"
        echo "        captured: $got"
        echo "        (upstream changed, or partial download — NOT staging this row)"
        DRIFT=$((DRIFT+1)); continue
    fi

    if ! extract "$arc" "$atype" "$WORK/$id"; then
        FAIL=$((FAIL+1)); continue
    fi

    if [ -d "$FIXUPS/$id" ]; then
        # Overlay our corpus-side fixes on top of the extracted tree.
        # Two forms:
        #   <path>.patch — unified diff applied to work/<id>/<path> (preferred
        #                  for edits to THIRD-PARTY files: we commit only our
        #                  delta, never the program's bytes)
        #   <path>       — file copied verbatim (for files we author/source
        #                  cleanly ourselves: missing libs, artifacts)
        ( cd "$FIXUPS/$id" && find . -type f ! -name 'README.md' -print0 | while IFS= read -r -d '' f; do
            case "$f" in
                *.patch)
                    tgt="${f%.patch}"
                    if ! patch -s -p0 -d "$WORK/$id" "$tgt" < "$f"; then
                        echo "      ! fixup patch FAILED: $f"
                    fi ;;
                *)
                    mkdir -p "$WORK/$id/$(dirname "$f")"
                    cp "$f" "$WORK/$id/$f" ;;
            esac
          done )
        echo "      + fixups overlaid"
    fi

    echo "OK    $id ($atype, sha ok)"
    OK=$((OK+1))
done < "$MANIFEST"

echo "----------------------------------------------------------------"
echo "fetch: $OK staged / $TOTAL rows  (drift=$DRIFT fail=$FAIL skip-noarchive=$SKIP)"
[ "$DRIFT" -eq 0 ] && [ "$FAIL" -eq 0 ]
