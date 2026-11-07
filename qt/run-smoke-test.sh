#!/bin/bash
# Headless smoke test for the Qt tier.
#
# Usage: run-smoke-test.sh <build-dir> <source-dir>
#
# Loads a real book, renders a report for it through gnucash-cli, runs the
# event-load self-test, and fails unless every part reports success. This is
# what CI runs, and it is deliberately the same command a developer runs.
set -u

BUILD=${1:?usage: run-smoke-test.sh <build-dir> <source-dir>}
SRC=${2:?usage: run-smoke-test.sh <build-dir> <source-dir>}

BOOK="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
SCHEMAS="$BUILD/share/glib-2.0/schemas"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnucash-qt" ] || fail "gnucash-qt was not built"
[ -x "$BUILD/bin/gnucash-cli" ] || fail "gnucash-cli was not built (reports need it)"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

# The build tree ships schema XML but does not compile it; without this every
# preference lookup fails and reports produce nothing.
[ -f "$SCHEMAS/gschemas.compiled" ] || glib-compile-schemas "$SCHEMAS" || fail "cannot compile schemas"

# The self-test flips a real preference and flips it back. Point GSettings at
# the build tree's schemas (the prefix is not installed) and at the memory
# backend, so a test run can never write the developer's settings.
out=$(GSETTINGS_BACKEND=memory \
      GSETTINGS_SCHEMA_DIR="$SCHEMAS" \
      QT_QPA_PLATFORM=offscreen \
      QTWEBENGINE_DISABLE_SANDBOX=1 \
      QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu" \
      LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
      "$BUILD/bin/gnucash-qt" \
      --self-test \
      "$BUILD/lib/gnucash/libgncmod-backend-xml.so" \
      "$BOOK" \
      "Trial Balance" 2>&1)
status=$?

echo "$out"

[ "$status" -eq 0 ] || fail "shell exited $status"
echo "$out" | grep -q "events=20/20 in_order=1 on_qt_thread=1" || fail "event load test"
echo "$out" | grep -q "pane_ok=1" || fail "report pane did not render"
echo "$out" | grep -q "tabs_ok=1" || fail "report pages are not tabs over the account page"
echo "$out" | grep -q "keys_ok=1" || fail "the audited keyboard bindings are missing"
echo "$out" | grep -q "model_ok=1" || fail "account model did not follow an edit"
echo "$out" | grep -q "total_ok=1" || fail "account totals did not follow a split"
echo "$out" | grep -q "period_ok=1" || fail "period total did not separate from the balance"
echo "$out" | grep -q "prefs_ok=1" || fail "the negative-in-red preference was not honoured"
echo "$out" | grep -q "prefs_ui_ok=1" || fail "the preferences dialog did not write its keys"
echo "$out" | grep -qE "reports=[1-9][0-9]*" || fail "no reports listed from the helper"
echo "$out" | grep -qE "report bytes: [1-9][0-9]+" || fail "report was not generated"

echo "qt smoke test passed"
