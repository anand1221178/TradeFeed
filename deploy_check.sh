#!/usr/bin/env bash
# Environment check + build + verify for the Linux deployment box.
# Usage: ./deploy_check.sh [--bench]
set -uo pipefail

RED=$'\033[0;31m'; GRN=$'\033[0;32m'; YEL=$'\033[0;33m'; RST=$'\033[0m'
ok()   { echo "${GRN}  ok${RST}   $*"; }
warn() { echo "${YEL} warn${RST}   $*"; }
fail() { echo "${RED} FAIL${RST}   $*"; FAILED=1; }
FAILED=0

echo "=== toolchain ==="

if command -v g++ >/dev/null; then
    GV=$(g++ -dumpversion | cut -d. -f1)
    [ "$GV" -ge 10 ] && ok "g++ $(g++ -dumpversion) (C++20 capable)" \
                     || fail "g++ $(g++ -dumpversion) too old, need >= 10"
elif command -v clang++ >/dev/null; then
    CV=$(clang++ -dumpversion | cut -d. -f1)
    [ "$CV" -ge 12 ] && ok "clang++ $(clang++ -dumpversion)" \
                     || fail "clang++ $(clang++ -dumpversion) too old, need >= 12"
else
    fail "no C++ compiler found"
fi

if command -v cmake >/dev/null; then
    CMV=$(cmake --version | head -1 | awk '{print $3}')
    CMAJ=$(echo "$CMV" | cut -d. -f1); CMIN=$(echo "$CMV" | cut -d. -f2)
    { [ "$CMAJ" -gt 3 ] || { [ "$CMAJ" -eq 3 ] && [ "$CMIN" -ge 20 ]; }; } \
        && ok "cmake $CMV" || fail "cmake $CMV too old, need >= 3.20"
else
    fail "cmake not found"
fi

echo
echo "=== hardware ==="

NCPU=$(nproc)
ok "$NCPU online CPUs"
[ "$NCPU" -lt 3 ] && fail "need >= 3 cores (engine pins to 1, gateway to 2)"

if grep -q constant_tsc /proc/cpuinfo && grep -q nonstop_tsc /proc/cpuinfo; then
    ok "invariant TSC (constant_tsc + nonstop_tsc) — rdtsc timing is reliable"
else
    warn "no invariant TSC — rdtsc calibration may drift with frequency scaling"
fi

MEMGB=$(awk '/MemTotal/{printf "%.0f", $2/1024/1024}' /proc/meminfo)
[ "$MEMGB" -ge 1 ] && ok "${MEMGB}GB RAM (need ~150MB: 48MB book + 64MB pool + 32MB rings)" \
                   || warn "low memory"

echo
echo "=== cpu topology (pin engine and gateway to distinct PHYSICAL cores) ==="
if command -v lscpu >/dev/null; then
    lscpu -e=CPU,CORE,SOCKET 2>/dev/null | head -8
    SIBS=$(cat /sys/devices/system/cpu/cpu1/topology/thread_siblings_list 2>/dev/null || echo "?")
    echo "  cpu1 thread siblings: $SIBS"
    [[ "$SIBS" == *","* ]] && warn "cpu1 is hyperthreaded with $SIBS — engine(1)/gateway(2) may share L1/L2"
else
    warn "lscpu not installed (pacman -S util-linux)"
fi

echo
echo "=== tuning (optional, improves tail latency) ==="

GOV=$(cat /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
[ "$GOV" = "performance" ] && ok "governor: performance" \
                           || warn "governor: $GOV — set with: sudo cpupower frequency-set -g performance"

CMDLINE=$(cat /proc/cmdline)
[[ "$CMDLINE" == *isolcpus* ]] && ok "isolcpus set" \
    || warn "no isolcpus — add 'isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2' to kernel cmdline"

RTRUN=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo "?")
[ "$RTRUN" = "-1" ] && ok "RT throttling disabled" \
                    || warn "RT throttle at ${RTRUN}us/1000000us — causes periodic stalls under SCHED_FIFO"

[ "$(id -u)" -eq 0 ] && ok "running as root — SCHED_FIFO will apply" \
                     || warn "not root — SCHED_FIFO will be skipped (sudo, or: setcap cap_sys_nice+ep ./build/tradefeed)"

echo
echo "=== build ==="
rm -rf build && mkdir -p build && cd build || exit 1
if ! cmake .. > /tmp/tf_cmake.log 2>&1; then
    fail "cmake configure failed"; tail -20 /tmp/tf_cmake.log; exit 1
fi
ok "cmake configured"

if ! make -j"$NCPU" > /tmp/tf_build.log 2>&1; then
    fail "build failed"; grep -E "error|Error" /tmp/tf_build.log | head -30; exit 1
fi
WARNS=$(grep -c "warning:" /tmp/tf_build.log || true)
[ "$WARNS" -eq 0 ] && ok "built clean, 0 warnings" || warn "built with $WARNS warnings"
grep "warning:" /tmp/tf_build.log | head -10

echo
echo "=== functional check ==="

./tradefeed 9999 > /tmp/tf_run.log 2>&1 &
PID=$!
sleep 1
if kill -0 $PID 2>/dev/null; then
    ok "exchange started and is listening"
    START=$(date +%s%N)
    kill -TERM $PID; wait $PID 2>/dev/null
    MS=$(( ($(date +%s%N) - START) / 1000000 ))
    [ "$MS" -lt 1000 ] && ok "clean shutdown in ${MS}ms" || warn "shutdown took ${MS}ms"
else
    fail "exchange died on startup"; cat /tmp/tf_run.log
fi
grep -i warning /tmp/tf_run.log | sed 's/^/       /'

echo
if [ "${1:-}" = "--bench" ]; then
    echo "=== benchmarks ==="
    echo "--- order book ---"; ./bench_orderbook
    echo; echo "--- ring buffer ---"; ./bench_ring
else
    echo "Run './deploy_check.sh --bench' to execute benchmarks."
    echo "For best numbers:  sudo taskset -c 3 ./build/bench_orderbook"
fi

echo
[ "$FAILED" -eq 0 ] && echo "${GRN}=== READY ===${RST}" || echo "${RED}=== BLOCKED: fix FAIL items above ===${RST}"
exit $FAILED
