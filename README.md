# Critter predictability test kit (Raspberry Pi 5)

This kit answers one question for the Critter: **what has the biggest impact on how predictably the device can take a temperature sample on time?** It uses the same idea as the course's `sleeploop.c`. A *sampler* thread asks the OS to wake it every *P* µs, the way the Critter's I/O unit must sample at a fixed rate, and records how late every wake-up actually is. The three Critter units then run as background load so their effect on that one periodic task can be measured, alone and combined. On top of that, the factors listed in the assignment (RAM, number and speed of cores, networking, OS priority requests) are varied one group at a time.

Because the Critter is **soft** real-time, the numbers that matter are the tail of the latency distribution (p99, p99.9, max) and the deadline-miss rate, not the mean. A system with a good average but occasional 20 ms stalls is less predictable than one that is always 80 µs late.

## Files

| File | Purpose |
|---|---|
| `critter_predict.c` | The harness: sampler + Critter load units + OS controls |
| `run_experiments.sh` | Runs every experiment group and collects CSVs |
| `plot_results.py` | Bar charts, tail curves, sleeploop-style time series, impact ranking |
| `Makefile` | `make` builds `critter_predict` |

## Build and quick start

```bash
sudo apt install build-essential python3-matplotlib   # once
make
./critter_predict -h                                  # all options
./critter_predict -L test-baseline                     # 10 000 samples @ 1 ms, idle
./critter_predict -L test-mix -c 2 -d 2 -i 1 -N 1      # whole Critter running
sudo ./run_experiments.sh                              # full sweep (~15-25 min)
```

Running the sweep without `sudo` still works; the real-time priority, negative-nice and CPU-frequency experiments are skipped. Results go to `results/`: `summary.csv` (one row per run), `samples/<label>.csv` (every sample), `environment.txt`, `throttle.log`, and `plots/`.

## The Critter units used as load

The **compute unit** (`-c`) is the HVAC prediction prototype. It fits an 8th-order autoregressive model to a sliding window of 512 temperature samples using least squares, then runs a Monte Carlo forecast of the probability that the room passes 27 °C. Its footprint is a few KB, so it is CPU/FPU bound. The **data unit** (`-d`) is the memory-focused prototype. It removes outliers with a median/MAD filter, summarizes the batch (min, max, mean, variance), and updates a large history buffer (`-w` MB per thread, default 32) one cache line at a time, which streams the working set through the shared L3 and DRAM. The **I/O unit** (`-i`) reads the Pi's real thermal sensor at `/sys/class/thermal/thermal_zone0/temp` as fast as it can and logs readings to a file; `-S` adds `fsync` so writes really reach the SD card or SSD. The **network unit** (`-N`) sends 1 KB UDP summary packets, as a set of Critters reporting to a collector. With the default target `127.0.0.1` a local sink is started; point `-T` at another machine to include the real Ethernet/Wi-Fi driver and interrupts.

The synthetic temperature signal is a 22 °C setpoint with a 15-minute HVAC cycle, slow drift, sensor noise and rare glitches for the outlier filter to remove.

## How each assignment factor is varied

| Factor | How | Group in `run_experiments.sh` |
|---|---|---|
| Program type | `-c`, `-d`, `-i` (`-S`), `-N` alone, then mixed | `type` |
| OS priority requests | `-P` nice, `-s fifo -P 80`, plus `-a/-A` pinning, `-l` mlockall, `-t` timer slack | `prio` |
| Number of cores | `taskset -c 0-(n-1)` around the whole run | `cores` |
| Core speed | cpufreq governor and clamped `scaling_max_freq` (min / mid / max) | `freq` |
| Amount of RAM | `-H` memory hog holding 0/50/75/90 % of available RAM | `ram` |
| Networking | `-N` (and `-T` to a remote host) | `type`, plus inside every `MIX` run |
| Measurement method | `usleep` vs `nanosleep` vs absolute `clock_nanosleep`, `-x` live printing | `sleep` |
| Sample rate | 100 µs, 1 ms, 10 ms | `period` |
| Real sampling work | `-r` reads the sensor each period and times the read | `sensor` |

To change these at boot instead of in software, `maxcpus=N` in `/boot/firmware/cmdline.txt` limits the cores the kernel brings up. `isolcpus=3` keeps the scheduler off core 3 so the sampler can have it alone. `total_mem=1024` in `/boot/firmware/config.txt` is supported on many Pi models for limiting RAM. Confirm with `nproc` and `free -m` after rebooting, because firmware support varies. A PREEMPT_RT kernel is the next step past what the stock kernel can do.

## Reading the output

Each run prints the wake-up latency distribution, the number of soft-deadline misses (default deadline = one period, i.e. the sample slid into the next slot), the achieved sample rate, and the sampler thread's context switches and page faults. It also prints CPU temperature and clock at start and end, and how much work each load unit got done. The last item matters because raising the sampler's priority is only a fair win if the Critter's other units still make reasonable progress.

In `plots/`, the **tail** plots are the most informative. Each curve shows the fraction of samples that were at least *x* µs late. A curve that drops steeply at the left is predictable; one with a long flat shelf to the right is not. The **time-series** plots are the classic sleeploop picture and show whether bad samples are random, periodic (a timer or kernel thread) or clustered (writeback, reclaim, throttling). `impact_ranking.png` orders the groups by how far they moved p99.9 latency. Treat it as a first sort only, since groups use different background loads.

## Things that will confuse your results if you don't control them

**Heat.** The Pi 5 throttles its clock around 80–85 °C, and the Critter's own workloads generate that heat. The sweep waits for the SoC to cool below `COOL_C` (55 °C) between runs and logs `vcgencmd get_throttled` after each one. A non-zero value means that run was throttled, so use a fan or the active cooler.

**The observer effect.** Printing every sample inside the timed loop, as a simple sleeploop does, is I/O that becomes part of the measurement. The harness stores samples in pre-touched memory and writes them after the loop; `-x` deliberately puts the `printf` back so you can measure the difference. Running over SSH or in a desktop terminal changes that cost too.

**Relative sleeps drift.** `usleep(1000)` sleeps *at least* 1 ms after the call, so latency and loop overhead accumulate, and the achieved rate falls below 1000 Hz even with no load. Absolute `clock_nanosleep` (`-m abs`) holds the schedule. Watch `achieved_hz` in the summary.

**Timer slack.** Normal Linux threads get 50 µs of timer slack, so a SCHED_OTHER sampler is routinely woken up to 50 µs late on purpose. Real-time threads get none. `-t 1` shows this effect on its own, separate from priority.

**Idle is not the best case.** An idle Pi lets cores drop into idle states and lower clocks, and waking from them takes time. Median latency on an idle system can be worse than under light load. The `freq-gov_*_idle` runs separate this from load effects.

**SD card wear and OOM.** `-S` writes continuously, so keep runs short or point `-f` at a USB/NVMe drive or tmpfs. The 90 % RAM run may push the system into swap or the OOM killer, which is itself a result. Check `dmesg | grep -i oom` if a run is missing.

Repeat each configuration several times (`KEEP=1 OUT=results ./run_experiments.sh type` appends) before drawing conclusions from the max column. A single worst-case sample is noisy by nature.

## What to expect (hypotheses to confirm with your data)

These are patterns typically seen on a stock, non-RT Raspberry Pi OS kernel. Your measurements should decide.

Competition for the CPU under the default scheduler is usually the largest effect. When runnable compute or data threads outnumber free cores, a SCHED_OTHER sampler can wait milliseconds for its turn. Fewer cores makes this worse, which is why the `cores` group often looks dramatic. Requesting a real-time priority (`-s fifo`) usually collapses that tail by an order of magnitude or more. This is why OS priority is often the single biggest lever; nice values help far less. After priority is fixed, what remains comes from things the scheduler cannot preempt. These include interrupt and kernel work from storage writeback (`-S`) and networking, contention in the shared L3/DRAM from the data unit, and, once RAM runs out, page reclaim and swapping. Paging can produce the worst single stalls of all, and `mlockall` (`-l`) protects the sampler from it. CPU speed mostly scales how long the *work* takes (see `work_p99_us` with `-r`) and matters less for wake-up latency, except through idle states and thermal throttling.

For the write-up, a useful framing is that the mean barely moves between most configurations, while the tail moves by orders of magnitude. That gap is the predictability problem, and oversampling can only hide it if you know how long the tail is.

## CPU preflight (added after the first Pi 5 run)

The first sweep silently ran on a single CPU (`online_cpus = allowed_cpus = 1` in every row), which invalidated the core-count experiment. `run_experiments.sh` now checks before anything runs:

1. If cores exist but are offline, it brings them online (needs `sudo`).
2. If the shell inherited a restricted CPU mask, it widens it.
3. If fewer CPUs than expected are still usable (4 on a Pi 5), it stops and prints likely causes (`maxcpus=` / `nr_cpus=` in the kernel command line, offlined cores, container limits) plus recent kernel messages. Override with `ALLOW_FEWER_CPUS=1` (single-core results only).

Pinned runs now use the real last CPU for the sampler and keep the load on the others. The `cores` group sweeps 1..N cores with the same 7-thread load (`cores-N`) and with N-1 compute threads so one core stays free (`coresfit-N`). After the sweep, `cpu_check.txt` compares each run's reported CPU count with what it should have been.

## Mechanism tests (`mech` group, `-Q` attribution mode)

Added so that the causal explanations in the paper can be tested instead of inferred.

`-Q` records, for every sample, how long the sampler thread was runnable but waiting for a CPU
(from `/proc/thread-self/schedstat`). With `-r` it also splits each sensor read into CPU time, runqueue
wait and *blocked* time (neither running nor runnable), and counts voluntary and involuntary context
switches. A voluntary switch means the thread went to sleep itself (blocked on a lock or device);
an involuntary one means it was preempted.

```bash
sudo ./run_experiments.sh mech          # about 5 minutes
```

| Question | Runs | What to look for |
|---|---|---|
| Is the ~4 ms late-wake-up ceiling CPU waiting? | `mech-q_c1/2/4/8` | `median wait/lat` near 1 means the lateness is runqueue wait; does the ceiling change with thread count? |
| What causes the sensor-read stall? | `mech-sens_*` remove one contender at a time (compute, data, network, I/O unit) | which load makes the stall appear; the I/O unit alone (`i1`, `i2`) tests the shared-sysfs-node hypothesis |
| Is the stall blocking or CPU wait? | same runs with SCHED_FIFO (`mech-sens_fifo_*`) | stalls that survive FIFO with low CPU and wait share and a voluntary switch are blocking |

Results: `results/mechanism_report.txt`, `results/mechanism_summary.csv`, `results/plots/mechanism_*.png`.
Run it only after the CPU preflight reports all four cores usable.

To build on a machine without the Pi thermal node (testing only):
`gcc -O2 -pthread -DTHERMAL_PATH='"/proc/loadavg"' -o critter_predict critter_predict.c -lm`
