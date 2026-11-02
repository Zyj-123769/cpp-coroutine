#!/usr/bin/env bash
# 跑全部测试（不含需要交互的 echo_server）
set -u
cd "$(dirname "$0")/.."

TESTS="test_fiber test_scheduler test_timer test_ioscheduler test_hook
       test_io_concurrency test_fiber_exception test_stack_overflow test_multithread"

fail=0
for t in $TESTS; do
    echo "=== $t ==="
    timeout 30 "./$t" > /tmp/out_${t}.log 2>&1
    rc=$?
    echo "exit=$rc"
    tail -4 /tmp/out_${t}.log
    if [ "$rc" -ne 0 ]; then fail=1; fi
done

echo "===================="
[ "$fail" -eq 0 ] && echo "ALL TESTS PASS" || echo "SOME TESTS FAILED"
exit "$fail"