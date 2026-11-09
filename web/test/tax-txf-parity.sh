#!/bin/bash
# TXF file parity: the Scheme tax report's export versus the native one.
#
# Usage: tax-txf-parity.sh <guile-build> <native-build> <source-dir>
#                          [book] [start] [end]
#
# The US Tax Schedule report has a second output: the .txf file TurboTax
# imports.  The Scheme report writes it through its export thunk; the native
# route returns the same file for ?format=txf.  This script runs both on the
# same book and period, canonicalises their line endings (the Scheme export
# uses CRLF, so the native one does too; the comparison is on the bytes after
# CRLF -> LF), and diffs them.  It prints the diff, so a mismatch names the
# record and field that differ rather than only a checksum.
#
# The book defaults to one make-tax-book.py generates in a temporary
# directory; the period defaults to that book's 2026 tax year.  The Scheme
# side needs its From/To pinned because gnucash-cli can only run a report with
# its default options, so the script writes a saved-report variant into the
# oracle build's user data directory and restores it on exit, exactly as
# report-parity.sh does.
#
# Exit status: 0 when the files match, 1 otherwise.
set -u

GUILE_BUILD=${1:?usage: tax-txf-parity.sh <guile-build> <native-build> <source-dir> [book] [start] [end]}
NATIVE_BUILD=${2:?usage: tax-txf-parity.sh <guile-build> <native-build> <source-dir> [book] [start] [end]}
SRC=${3:?usage: tax-txf-parity.sh <guile-build> <native-build> <source-dir> [book] [start] [end]}
BOOK=${4:-}
START=${5:-2026-01-01}
END=${6:-2026-12-31}

CLI=${GNC_CLI:-$GUILE_BUILD/bin/gnucash-cli}
WEBDBIN="$NATIVE_BUILD/bin/gnc-webd"
BACKEND="$NATIVE_BUILD/lib/gnucash/libgncmod-backend-xml.so"
SAVED_REPORTS="$GUILE_BUILD/gnc_data_home/saved-reports-2.8"
VARIANT_NAME="Native tax TXF parity"
REPORT_GUID="f8921f4e5c284d7caca81e239f468a68"
REPORT_NAME="Tax Schedule Report/TXF Export"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$CLI" ] || fail "no gnucash-cli at $CLI (need a WITH_GNUCASH Guile build, or set GNC_CLI)"
[ -x "$WEBDBIN" ] || fail "gnc-webd was not built at $WEBDBIN"

TMP=$(mktemp -d)
SERVER_PID=
SAVED_REPORTS_BACKUP=
SAVED_REPORTS_WRITTEN=
cleanup() {
    kill "$SERVER_PID" 2>/dev/null
    if [ -n "$SAVED_REPORTS_WRITTEN" ]; then
        if [ -f "$SAVED_REPORTS_BACKUP" ]; then
            mv -f "$SAVED_REPORTS_BACKUP" "$SAVED_REPORTS"
        else
            rm -f "$SAVED_REPORTS"
        fi
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ -z "$BOOK" ]; then
    BOOK="$TMP/tax-book.gnucash"
    python3 "$SRC/web/test/make-tax-book.py" "$BOOK" > /dev/null \
        || fail "make-tax-book.py failed"
fi
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

START_T64=$(date -u -d "$START 12:00:00" +%s) \
    || fail "cannot parse start date '$START' (want YYYY-MM-DD)"
END_T64=$(date -u -d "$END 12:00:00" +%s) \
    || fail "cannot parse end date '$END' (want YYYY-MM-DD)"

# ------------------------------------------------------------------ oracle
mkdir -p "$(dirname "$SAVED_REPORTS")"
if [ -f "$SAVED_REPORTS" ]; then
    SAVED_REPORTS_BACKUP="$TMP/saved-reports.orig"
    cp "$SAVED_REPORTS" "$SAVED_REPORTS_BACKUP"
fi
SAVED_REPORTS_WRITTEN=1
cat > "$SAVED_REPORTS" <<EOF
;; Written by web/test/tax-txf-parity.sh; removed when the run ends.
(let ()
  (define (options-gen)
    (let ((options (gnc:report-template-new-options/report-guid
                    "$REPORT_GUID" "$REPORT_NAME")))
      (gnc-set-option (gnc:optiondb options) "General" "From"
                      (cons 'absolute $START_T64))
      (gnc-set-option (gnc:optiondb options) "General" "To"
                      (cons 'absolute $END_T64))
      options))
  (gnc:define-report
    'version 1
    'name "$VARIANT_NAME"
    'report-guid "9e9e9e9e9e9e9e9e9e9e9e9e9e9e9e9e"
    'parent-type "$REPORT_GUID"
    'options-generator options-gen
    'menu-path (list gnc:menuname-custom)
    'renderer (gnc:report-template-renderer/report-guid
               "$REPORT_GUID" "$REPORT_NAME")))
EOF

SCHEMAS="$GUILE_BUILD/share/glib-2.0/schemas"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""
CCACHE=$(ls -d "$GUILE_BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

echo "oracle: $CLI ($VARIANT_NAME)"
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$GUILE_BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$GUILE_BUILD/lib:$GUILE_BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$GUILE_BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$GUILE_BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    "$CLI" "$BOOK" -R run --name "$VARIANT_NAME" --export-type TXF \
    --output-file "$TMP/scheme.txf" > "$TMP/scheme.log" 2>&1 \
    || { cat "$TMP/scheme.log" >&2; fail "the Scheme TXF export failed"; }
[ -s "$TMP/scheme.txf" ] || fail "the Scheme TXF export is empty"

# ------------------------------------------------------------------ native
NATIVE_SCHEMAS="$NATIVE_BUILD/share/glib-2.0/schemas"
if [ ! -f "$NATIVE_SCHEMAS/gschemas.compiled" ]; then
    NATIVE_SCHEMAS="$GUILE_BUILD/share/glib-2.0/schemas"
fi
[ -f "$NATIVE_SCHEMAS/gschemas.compiled" ] || NATIVE_SCHEMAS=""
NATIVE_CCACHE=$(ls -d "$NATIVE_BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

BACKEND_ARGS=()
[ -f "$BACKEND" ] && BACKEND_ARGS=(--backend "$BACKEND")

env GNC_UNINSTALLED=YES GNC_BUILDDIR="$NATIVE_BUILD" \
    ${NATIVE_SCHEMAS:+GSETTINGS_SCHEMA_DIR="$NATIVE_SCHEMAS"} \
    LD_LIBRARY_PATH="$NATIVE_BUILD/lib:$NATIVE_BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$NATIVE_BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$NATIVE_BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${NATIVE_CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    "$WEBDBIN" "${BACKEND_ARGS[@]}" "$BOOK" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

URL=""
for _ in $(seq 1 300); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$TMP/server.log" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$TMP/server.log" >&2; fail "gnc-webd did not start"; }

REPORT_PATH="Tax%20Schedule%20Report/TXF%20Export"
curl -fsS "${URL%/}/report/$REPORT_PATH?start=$START&end=$END&format=txf" \
    -o "$TMP/native.txf" \
    || { cat "$TMP/server.log" >&2; fail "the native TXF request failed"; }
[ -s "$TMP/native.txf" ] || fail "the native TXF export is empty"

# ----------------------------------------------------------------- compare
python3 - "$TMP/scheme.txf" "$TMP/scheme.lf" <<'PY'
import sys
data = open(sys.argv[1], "rb").read().replace(b"\r\n", b"\n")
open(sys.argv[2], "wb").write(data)
PY
python3 - "$TMP/native.txf" "$TMP/native.lf" <<'PY'
import sys
data = open(sys.argv[1], "rb").read().replace(b"\r\n", b"\n")
open(sys.argv[2], "wb").write(data)
PY

echo "scheme TXF bytes: $(wc -c < "$TMP/scheme.txf")"
echo "native TXF bytes: $(wc -c < "$TMP/native.txf")"
if diff -u "$TMP/scheme.lf" "$TMP/native.lf" > "$TMP/txf.diff"; then
    echo "TXF files match after CRLF canonicalisation"
    exit 0
fi
cat "$TMP/txf.diff" >&2
fail "the native TXF export does not match the Scheme export"
