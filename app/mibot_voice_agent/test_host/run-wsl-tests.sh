#!/bin/sh
# Run every host test binary in build-wsl and report pass/fail per test.
cd "$(dirname "$0")/build-wsl" || exit 1
status=0
for t in test_*; do
  if [ -f "$t" ] && [ -x "$t" ]; then
    printf '%s: ' "$t"
    if ./"$t" > /tmp/hosttest.log 2>&1; then
      tail -1 /tmp/hosttest.log
    else
      echo FAILED
      tail -5 /tmp/hosttest.log
      status=1
    fi
  fi
done
exit $status
