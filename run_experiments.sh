#!/usr/bin/env bash
# run_experiments.sh -- sweep the factors that affect Critter predictability on a Pi 5.
#
#   ./run_experiments.sh                 run every group (non-root parts only)
#   sudo ./run_experiments.sh            also runs fifo / negative nice / cpufreq tests
#   sudo ./run_experiments.sh type prio  run only the named groups
#
# Groups:  type    program type of the background load (compute / data / io / net)
#          prio    OS priority requests, pinning, mlock, timer slack
#          cores   number of cores (taskset)
#          freq    core speed and cpufreq governor            (root)
#          ram     available RAM (memory hog)
#          sleep   sleep API and the observer effect of printing
#          period  sample period
#          sensor  sampler actually reads the thermal sensor each period
#          mech    mechanism tests (-Q): is lateness CPU wait or blocking? who causes the
#                  sensor-read stall? does the ~4 ms ceiling depend on thread count?
#
# Environment overrides: N=10000 PERIOD=1000 OUT=results COOL_C=55 KEEP=0
#                        ALLOW_FEWER_CPUS=1   run even if not all cores are online
#
# CPU preflight: before anything runs, the script checks that every core is
# online and usable. Offline cores are brought online (root), a restricted
# affinity mask is widened, and if cores are still missing the script stops
# with a diagnosis instead of silently measuring a single-core system.
set -u
cd "$(dirname "$0")"

BIN=./critter_predict
OUT=${OUT:-results}
N=${N:-10000}
PERIOD=${PERIOD:-1000}
COOL_C=${COOL_C:-55}
MIX="-c 2 -d 2 -i 1 -N 1"          # "whole Critter" background load
CPUFREQ=/sys/devices/system/cpu/cpufreq/policy0
THERM=/sys/class/thermal/thermal_zone0/temp

# ------------------------------------------------------------ CPU preflight
cpu_count() {   # "0-3,5" -> 5
    local n=0 part a b
    local IFS=','
    for part in $1; do
        [ -z "$part" ] && continue
        if [[ $part == *-* ]]; then a=${part%-*}; b=${part#*-}; n=$((n + b - a + 1)); else n=$((n + 1)); fi
    done
    echo "$n"
}
cpu_sys() { cat "/sys/devices/system/cpu/$1" 2>/dev/null || echo ""; }
allowed_list() { awk '/Cpus_allowed_list/ {print $2}' /proc/self/status; }

CPU_PRESENT=$(cpu_sys present)
CPU_ONLINE=$(cpu_sys online)
NPRESENT=$(cpu_count "${CPU_PRESENT:-0}")
NONLINE=$(cpu_count "${CPU_ONLINE:-0}")
EXPECT=$NPRESENT
grep -aq "Raspberry Pi 5" /proc/device-tree/model 2>/dev/null && EXPECT=4

# 1) cores that exist but are offline: try to bring them online (needs root)
if [ "$NONLINE" -lt "$NPRESENT" ]; then
    echo "!! CPU preflight: $NONLINE of $NPRESENT present CPUs are online (online=$CPU_ONLINE present=$CPU_PRESENT)"
    if [ "$(id -u)" -eq 0 ]; then
        for f in /sys/devices/system/cpu/cpu[0-9]*/online; do
            [ -e "$f" ] && [ "$(cat "$f")" = 0 ] && echo 1 > "$f" && echo "   brought $(basename "$(dirname "$f")") online"
        done
        CPU_ONLINE=$(cpu_sys online); NONLINE=$(cpu_count "${CPU_ONLINE:-0}")
    else
        echo "   (run with sudo so offline cores can be brought online)"
    fi
fi

# 2) this shell inherited a restricted affinity (systemd, taskset, cpuset): widen it
if [ "$(cpu_count "$(allowed_list)")" -lt "$NONLINE" ]; then
    echo "!! CPU preflight: this shell is limited to CPUs $(allowed_list), widening to $CPU_ONLINE"
    taskset -pc "$CPU_ONLINE" $$ >/dev/null 2>&1 || echo "   could not widen affinity"
fi
NCPU=$(cpu_count "$(allowed_list)")
LAST=$((NCPU - 1))

# 3) still short: stop and explain, unless the user insists
if [ "$NCPU" -lt "$EXPECT" ] && [ "${ALLOW_FEWER_CPUS:-0}" != 1 ]; then
    echo
    echo "ERROR: only $NCPU of $EXPECT expected CPUs are usable, so the core-count and priority"
    echo "       experiments would be meaningless. Likely causes:"
    echo "  - kernel parameter in /boot/firmware/cmdline.txt: $(grep -o -E 'maxcpus=[0-9]+|nr_cpus=[0-9]+|nosmp|isolcpus=[^ ]*' /proc/cmdline | tr '\n' ' ')(none found if blank)"
    echo "  - cores offlined by a power-saving tool or an earlier experiment"
    echo "  - running in a container or VM with a CPU limit"
    echo "  Details: online=$CPU_ONLINE present=$CPU_PRESENT possible=$(cpu_sys possible) offline=$(cpu_sys offline)"
    echo "  Kernel messages:"; dmesg 2>/dev/null | grep -i -E "smp|psci|cpu[0-9]|secondary" | tail -8 | sed 's/^/    /'
    echo "  To run anyway (single-core results only): ALLOW_FEWER_CPUS=1 $0 $*"
    exit 1
fi
echo "CPU preflight OK: $NCPU CPUs usable (online=$CPU_ONLINE)"

SEL=("$@")
[ ${#SEL[@]} -eq 0 ] && SEL=(type prio cores freq ram sleep period sensor mech)

IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1

[ -x "$BIN" ] || make || exit 1
mkdir -p "$OUT/samples"
SUM="$OUT/summary.csv"
[ "${KEEP:-0}" = 1 ] || rm -f "$SUM" "$OUT"/samples/*.csv
: > "$OUT/throttle.log"

want() { local g; for g in "${SEL[@]}"; do [ "$g" = "$1" ] && return 0; done; return 1; }
temp_c() { if [ -r "$THERM" ]; then echo $(( $(cat "$THERM") / 1000 )); else echo 0; fi; }
throttled() { if command -v vcgencmd >/dev/null; then vcgencmd get_throttled; else echo "throttled=n/a"; fi; }

# wait for the SoC to cool so heat from one run does not bias the next
cooldown() {
    local i=0
    while [ "$(temp_c)" -gt "$COOL_C" ] && [ $i -lt 90 ]; do sleep 1; i=$((i + 1)); done
    sleep 1
}

# run LABEL "PREFIX" [critter_predict args...]   (later args override -n / -p)
run() {
    local label=$1 prefix=$2
    shift 2
    cooldown
    echo ">>> $label   ${prefix:+$prefix }$BIN $*"
    # shellcheck disable=SC2086
    $prefix "$BIN" -L "$label" -n "$N" -p "$PERIOD" \
        -o "$OUT/samples/$label.csv" -R "$SUM" "$@"
    echo "$label $(throttled) temp_after=$(temp_c)C" >> "$OUT/throttle.log"
}
root_run() {
    if [ $IS_ROOT = 1 ]; then run "$@"; else echo "--- skip $1 (needs sudo)"; fi
}

# ---------------------------------------------------------------- environment
{
    echo "date: $(date)"
    echo "uname: $(uname -a)"
    grep -m1 -i "model" /proc/cpuinfo
    tr -d '\0' < /proc/device-tree/model 2>/dev/null; echo
    grep -E "MemTotal|MemAvailable|SwapTotal" /proc/meminfo
    echo "page size: $(getconf PAGESIZE)"
    echo "governor: $(cat $CPUFREQ/scaling_governor 2>/dev/null)"
    echo "freqs: $(cat $CPUFREQ/scaling_available_frequencies 2>/dev/null)"
    echo "kernel preempt: $(uname -v | grep -oE 'PREEMPT[_A-Z]*' | head -1)"
    echo "cmdline: $(cat /proc/cmdline)"
    echo "cpu present=$(cpu_sys present) online=$(cpu_sys online) offline=$(cpu_sys offline) allowed=$(allowed_list) usable=$NCPU"
    echo "nproc: $(nproc)"
    echo "start temp: $(temp_c) C, $(throttled)"
    echo "sched_rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)"
} > "$OUT/environment.txt"

# -------------------------------------------------------------- program type
if want type; then
    run type-baseline     ""
    run type-compute4     "" -c 4
    run type-data4        "" -d 4
    run type-io2          "" -i 2
    run type-io2_fsync    "" -i 2 -S
    run type-net2         "" -N 2
    run type-mix          "" $MIX
fi

# ----------------------------------------------------- OS priority requests
if want prio; then
    run      prio-nice0                       "" $MIX
    run      prio-slack1                      "" $MIX -t 1
    root_run prio-nice-20                     "" $MIX -s other -P -20
    root_run prio-fifo80                      "" $MIX -s fifo -P 80
    if [ "$NCPU" -ge 2 ]; then
        # sampler alone on the last core, all load on the others
        PIN="-a $LAST -A 0-$((LAST - 1))"
        run      prio-pinned                      "" $MIX $PIN
        root_run prio-fifo80_pinned               "" $MIX -s fifo -P 80 $PIN
        root_run prio-fifo80_pinned_mlock_slack1  "" $MIX -s fifo -P 80 $PIN -l -t 1
    else
        echo "--- skip pinned runs (only one CPU)"
    fi
fi

# ---------------------------------------------------------- number of cores
if want cores; then
    for n in $(seq 1 "$NCPU"); do
        # same 7-thread load on n cores (oversubscribed unless n is large)
        run "cores-${n}" "taskset -c 0-$((n - 1))" $MIX
    done
    for n in $(seq 2 "$NCPU"); do
        # n-1 compute threads on n cores: one core stays free for the sampler
        run "coresfit-${n}" "taskset -c 0-$((n - 1))" -c $((n - 1))
    done
fi

# ----------------------------------------------------- core speed / governor
if want freq; then
    if [ $IS_ROOT = 1 ] && [ -d "$CPUFREQ" ]; then
        ORIG_GOV=$(cat $CPUFREQ/scaling_governor)
        ORIG_MAX=$(cat $CPUFREQ/scaling_max_freq)
        ORIG_MIN=$(cat $CPUFREQ/scaling_min_freq)
        restore_freq() {
            echo "$ORIG_MIN" > $CPUFREQ/scaling_min_freq
            echo "$ORIG_MAX" > $CPUFREQ/scaling_max_freq
            echo "$ORIG_GOV" > $CPUFREQ/scaling_governor
        }
        trap restore_freq EXIT

        if [ -r $CPUFREQ/scaling_available_frequencies ]; then
            mapfile -t F < <(tr ' ' '\n' < $CPUFREQ/scaling_available_frequencies | grep -v '^$' | sort -n)
        else
            F=("$(cat $CPUFREQ/cpuinfo_min_freq)" "$(cat $CPUFREQ/cpuinfo_max_freq)")
        fi
        FMIN=${F[0]}; FMAX=${F[${#F[@]} - 1]}; FMID=${F[$(( ${#F[@]} / 2 ))]}

        # governor: does letting idle cores clock down change wake-up latency?
        for gov in ondemand schedutil performance; do
            grep -qw "$gov" $CPUFREQ/scaling_available_governors || continue
            echo "$gov" > $CPUFREQ/scaling_governor
            run "freq-gov_${gov}_idle" ""
            run "freq-gov_${gov}_mix"  "" $MIX
        done

        # fixed clock: performance governor, max frequency clamped
        echo performance > $CPUFREQ/scaling_governor
        echo "$FMIN" > $CPUFREQ/scaling_min_freq
        for f in "$FMIN" "$FMID" "$FMAX"; do
            echo "$f" > $CPUFREQ/scaling_max_freq
            run "freq-$((f / 1000))MHz_mix" "" $MIX
        done
        restore_freq
        trap - EXIT
    else
        echo "--- skip freq group (needs sudo and cpufreq)"
    fi
fi

# ------------------------------------------------------------- available RAM
if want ram; then
    AVAIL_MB=$(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
    echo "MemAvailable: ${AVAIL_MB} MB"
    for pct in 0 50 75 90; do
        run "ram-hog${pct}pct" "" -d 2 -w 64 -H $(( AVAIL_MB * pct / 100 ))
    done
    echo "note: a missing ram run usually means the OOM killer took it (dmesg | grep -i oom)"
fi

# --------------------------------------------- sleep API and observer effect
if want sleep; then
    for m in usleep nanosleep abs; do
        run "sleep-${m}_idle" "" -m "$m"
        run "sleep-${m}_mix"  "" -m "$m" $MIX
    done
    # printing every sample inside the loop, as a naive sleeploop does
    run "sleep-liveprint_mix" "" $MIX -x > "$OUT/liveprint_stdout.txt"
fi

# ------------------------------------------------------------- sample period
if want period; then
    run period-100us_idle "" -p 100   -n 20000
    run period-100us_mix  "" -p 100   -n 20000 $MIX
    run period-1ms_idle   "" -p 1000
    run period-1ms_mix    "" -p 1000  $MIX
    run period-10ms_idle  "" -p 10000 -n 2000
    run period-10ms_mix   "" -p 10000 -n 2000 $MIX
fi

# ------------------------------------------- sampler does the real sensor read
if want sensor; then
    run      sensor-read_idle     "" -r -m abs
    run      sensor-read_mix      "" -r -m abs $MIX
    root_run sensor-read_fifo_mix "" -r -m abs $MIX -s fifo -P 80
fi

# --------------------------------------------- mechanism tests (attribution -Q)
if want mech; then
    # Q1/quantum: does the late-wake-up ceiling depend on how many threads share the CPU?
    for n in 1 2 4 8; do
        run "mech-q_c${n}" "" -Q -c "$n"
    done
    # Q2: who causes the sensor-read stall? Remove one contender at a time.
    run "mech-sens_none"    "" -Q -r -m abs
    run "mech-sens_c2"      "" -Q -r -m abs -c 2
    run "mech-sens_d2"      "" -Q -r -m abs -d 2
    run "mech-sens_n1"      "" -Q -r -m abs -N 1
    run "mech-sens_i1"      "" -Q -r -m abs -i 1
    run "mech-sens_i2"      "" -Q -r -m abs -i 2
    run "mech-sens_mixnoio" "" -Q -r -m abs -c 2 -d 2 -N 1
    run "mech-sens_mix"     "" -Q -r -m abs $MIX
    # same with real-time priority: if the stall survives, it is blocking, not CPU wait
    root_run "mech-sens_fifo_i1"      "" -Q -r -m abs -i 1 -s fifo -P 80
    root_run "mech-sens_fifo_mixnoio" "" -Q -r -m abs -c 2 -d 2 -N 1 -s fifo -P 80
    root_run "mech-sens_fifo_mix"     "" -Q -r -m abs $MIX -s fifo -P 80
fi

# verify each run really had the CPUs it was supposed to (columns: 1=label 17=allowed_cpus)
echo
awk -F, -v ncpu="$NCPU" 'NR>1 {
    want = ncpu
    if ($1 ~ /^cores-[0-9]+$/ || $1 ~ /^coresfit-[0-9]+$/) { split($1, a, "-"); want = a[2] }
    if ($17 != want) { bad++; printf "CPU MISMATCH: %s ran with %s CPUs, expected %s\n", $1, $17, want }
    n++ }
  END { if (bad) printf "!! %d of %d runs had the wrong CPU count -- do not trust those rows\n", bad, n;
        else printf "CPU check: all %d runs used the expected number of CPUs\n", n }' "$SUM" | tee "$OUT/cpu_check.txt"

echo
echo "Done. Summary: $SUM"
if command -v python3 >/dev/null && [ -f plot_results.py ]; then
    python3 plot_results.py "$OUT" || echo "plotting failed (sudo apt install python3-matplotlib)"
fi
if want mech && command -v python3 >/dev/null && [ -f analyze_mechanism.py ]; then
    python3 analyze_mechanism.py "$OUT" | tee "$OUT/mechanism_report.txt" || echo "mechanism analysis failed"
fi

# hand results back to the normal user when run with sudo
if [ $IS_ROOT = 1 ] && [ -n "${SUDO_USER:-}" ]; then
    chown -R "$SUDO_USER": "$OUT"
fi
