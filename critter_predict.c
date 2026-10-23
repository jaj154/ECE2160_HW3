/*
 * critter_predict.c
 *
 * Predictability test harness for the Critter machine-room HVAC monitor,
 * written for a Raspberry Pi 5 running Raspberry Pi OS (64-bit Linux).
 *
 * Idea (same as the course's sleeploop.c): a "sampler" thread asks the OS to
 * wake it every P microseconds, the way the Critter's I/O unit must take a
 * temperature sample at a fixed rate.  We record how LATE every wake-up is.
 * A perfectly predictable system has a tight, small latency distribution;
 * anything that disturbs predictability shows up as jitter, long tails and
 * missed (soft) deadlines.
 *
 * While the sampler runs, the three Critter units can be started as
 * background load, so each program type can be tested on its own or mixed:
 *
 *   compute unit (-c)  HVAC prediction: least-squares AR(8) model fit over a
 *                      sliding window + Monte Carlo forecast of the chance the
 *                      room passes 27 C.  CPU / FPU bound, small footprint.
 *   data unit    (-d)  Outlier removal (median / MAD filter) and summarizing
 *                      of a large history buffer.  Memory / cache bound.
 *   I/O unit     (-i)  High-rate reads of the Pi's real thermal sensor
 *                      (sysfs) and logging to a file, optionally fsync'd.
 *   network      (-N)  UDP summary packets, as a distributed set of Critters
 *                      reporting to each other / a collector.
 *   memory hog   (-H)  Holds N MB of RAM in use to emulate a board with less
 *                      memory (pushes the system toward reclaim / swap).
 *
 * OS controls: scheduling policy and priority, CPU pinning, mlockall, timer
 * slack and sleep method.  Core count and CPU speed are varied from outside
 * (taskset, cpufreq sysfs) by run_experiments.sh.
 *
 * Build:  make            (or: gcc -O2 -Wall -pthread critter_predict.c -o critter_predict -lm)
 * Run:    ./critter_predict -h
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_SEC   1000000000LL
#ifndef THERMAL_PATH   /* override with -DTHERMAL_PATH='"/path"' to test on other machines */
#define THERMAL_PATH "/sys/class/thermal/thermal_zone0/temp"
#endif
#define FREQ_PATH    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define MAX_THREADS  64

enum { SLEEP_USLEEP = 0, SLEEP_NANOSLEEP, SLEEP_ABS };
static const char *sleep_names[] = { "usleep", "nanosleep", "abs" };

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */
typedef struct {
    long period_us;
    long nsamples;
    int  sleep_mode;
    int  read_sensor;
    int  live_print;
    int  attrib;          /* -Q: scheduler-attribution columns */
    long deadline_us;
    long warmup_ms;

    int  n_compute, n_data, n_io, n_net;
    long ws_mb;
    long hog_mb;
    int  io_sync;
    char io_dir[256];
    char net_target[64];
    int  net_port;

    int  policy;
    int  prio;
    int  prio_set;
    int  sampler_cpu;
    char load_cpus[64];
    int  do_mlock;
    long timer_slack_ns;

    char csv_path[256];
    char summary_path[256];
    char label[64];
} config_t;

static config_t cfg = {
    .period_us = 1000, .nsamples = 10000, .sleep_mode = SLEEP_USLEEP,
    .deadline_us = -1, .warmup_ms = 1000,
    .ws_mb = 32, .io_dir = ".", .net_target = "127.0.0.1", .net_port = 5175,
    .policy = SCHED_OTHER, .sampler_cpu = -1, .timer_slack_ns = -1,
    .label = "run",
};

/* ------------------------------------------------------------------------ */
/* Shared state                                                             */
/* ------------------------------------------------------------------------ */
static atomic_int   g_stop;
static atomic_int   g_hog_ready;
static atomic_ulong g_compute_iters, g_data_batches, g_io_reads, g_net_pkts;

static pthread_t g_threads[MAX_THREADS];
static int       g_nthreads;

typedef struct {
    int64_t lat_ns;       /* actual wake time - intended wake time */
    int64_t work_ns;      /* time spent reading the sensor (-r) */
    int64_t interval_ns;  /* time between consecutive wake-ups */
    int32_t temp_mc;      /* sensor value in milli-degrees C (-r) */
    /* -Q attribution (zero unless enabled) */
    int64_t sleep_wait_ns; /* runnable-but-not-running time during sleep+wake */
    int64_t rd_cpu_ns;     /* CPU time consumed during the sensor read */
    int64_t rd_wait_ns;    /* runqueue wait time during the sensor read */
    int32_t rd_vcsw;       /* voluntary context switches during the read */
    int32_t rd_ivcsw;      /* involuntary context switches during the read */
} sample_t;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */
static inline int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * NS_PER_SEC + t->tv_nsec;
}

static inline int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ts_ns(&t);
}

static inline void ts_add_ns(struct timespec *t, int64_t ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= NS_PER_SEC) {
        t->tv_nsec -= NS_PER_SEC;
        t->tv_sec++;
    }
}

static long read_sysfs_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (!f)
        return -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static double read_temp_c(void)
{
    long v = read_sysfs_long(THERMAL_PATH);
    return v < 0 ? -1.0 : v / 1000.0;
}

/* Runqueue wait of the calling thread, from /proc/thread-self/schedstat:
 * fields are "cpu_ns wait_ns timeslices". Wait time is credited when the
 * thread is next switched in, so a read after a wake-up includes that wake-up. */
static int read_wait_ns(int fd, int64_t *wait)
{
    char b[128];
    unsigned long long run, w, sl;
    ssize_t n = pread(fd, b, sizeof b - 1, 0);
    if (n <= 0)
        return -1;
    b[n] = '\0';
    if (sscanf(b, "%llu %llu %llu", &run, &w, &sl) != 3)
        return -1;
    *wait = (int64_t)w;
    return 0;
}

static inline int64_t thread_cpu_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return ts_ns(&t);
}

static inline int stopping(void)
{
    return atomic_load_explicit(&g_stop, memory_order_relaxed);
}

static inline void count(atomic_ulong *c)
{
    atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
}

/* xorshift64 PRNG: cheap and deterministic, no shared state between threads */
static inline uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static inline double urand(uint64_t *s)
{
    return (double)(xorshift64(s) >> 11) * (1.0 / 9007199254740992.0);
}

static double gauss(uint64_t *s)
{
    double u1 = urand(s), u2 = urand(s);
    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/*
 * Synthetic machine-room temperature (deg C) at time t seconds:
 * setpoint + HVAC duty cycle (15 min) + slow drift + sensor noise
 * + rare sensor glitches, which the data unit must remove as outliers.
 */
static double synth_temp(uint64_t *s, double t)
{
    double v = 22.0 + 1.5 * sin(2.0 * M_PI * t / 900.0) + 0.0005 * t
             + 0.15 * gauss(s);
    if (urand(s) < 0.002)
        v += (urand(s) < 0.5 ? -1.0 : 1.0) * (10.0 + 20.0 * urand(s));
    return v;
}

/* cheaper version used to refill large buffers */
static inline float fast_temp(uint64_t *s)
{
    float v = 22.0f + 0.6f * (float)(urand(s) - 0.5);
    if ((xorshift64(s) & 1023) == 0)
        v += 25.0f;
    return v;
}

static int parse_cpu_list(const char *s, cpu_set_t *set)
{
    char buf[64], *save = NULL, *tok;
    CPU_ZERO(set);
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            for (int c = a; c <= b; c++)
                CPU_SET(c, set);
        } else if (sscanf(tok, "%d", &a) == 1) {
            CPU_SET(a, set);
        } else {
            return -1;
        }
    }
    return CPU_COUNT(set) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Compute unit: HVAC behaviour prediction                                  */
/* ------------------------------------------------------------------------ */
#define AR_W  512   /* history window (samples)     */
#define AR_P  8     /* autoregressive model order    */
#define AR_H  64    /* forecast horizon (samples)    */
#define AR_MC 64    /* Monte Carlo trajectories      */

static int solve_ar(double A[AR_P][AR_P], double b[AR_P], double x[AR_P])
{
    /* Gaussian elimination with partial pivoting */
    for (int c = 0; c < AR_P; c++) {
        int piv = c;
        for (int r = c + 1; r < AR_P; r++)
            if (fabs(A[r][c]) > fabs(A[piv][c]))
                piv = r;
        if (fabs(A[piv][c]) < 1e-12)
            return -1;
        if (piv != c) {
            for (int k = 0; k < AR_P; k++) {
                double t = A[c][k];
                A[c][k] = A[piv][k];
                A[piv][k] = t;
            }
            double t = b[c];
            b[c] = b[piv];
            b[piv] = t;
        }
        for (int r = c + 1; r < AR_P; r++) {
            double f = A[r][c] / A[c][c];
            for (int k = c; k < AR_P; k++)
                A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = AR_P - 1; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < AR_P; k++)
            s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return 0;
}

static void *compute_unit(void *arg)
{
    uint64_t rng = 0x9E3779B97F4A7C15ULL ^ ((uintptr_t)arg + 1) * 7919;
    double hist[AR_W], t = 0.0;
    volatile double sink = 0.0;

    for (int i = 0; i < AR_W; i++, t += 1.0)
        hist[i] = synth_temp(&rng, t);

    while (!stopping()) {
        /* slide the window: one new sample arrives */
        memmove(hist, hist + 1, (AR_W - 1) * sizeof(double));
        hist[AR_W - 1] = synth_temp(&rng, t);
        t += 1.0;

        double mean = 0.0;
        for (int i = 0; i < AR_W; i++)
            mean += hist[i];
        mean /= AR_W;

        /* normal equations for the AR(P) least-squares fit */
        double A[AR_P][AR_P] = {{0}}, b[AR_P] = {0}, coef[AR_P] = {0};
        for (int k = AR_P; k < AR_W; k++) {
            double y = hist[k] - mean;
            for (int i = 0; i < AR_P; i++) {
                double xi = hist[k - 1 - i] - mean;
                b[i] += xi * y;
                for (int j = 0; j < AR_P; j++)
                    A[i][j] += xi * (hist[k - 1 - j] - mean);
            }
        }
        for (int i = 0; i < AR_P; i++)
            A[i][i] += 1e-3;                 /* ridge term for stability */
        if (solve_ar(A, b, coef))
            continue;

        /* residual noise level */
        double rss = 0.0;
        for (int k = AR_P; k < AR_W; k++) {
            double pred = 0.0;
            for (int i = 0; i < AR_P; i++)
                pred += coef[i] * (hist[k - 1 - i] - mean);
            double e = (hist[k] - mean) - pred;
            rss += e * e;
        }
        double sigma = sqrt(rss / (AR_W - AR_P));

        /* Monte Carlo: probability the room exceeds 27 C within AR_H steps */
        int exceed = 0;
        for (int m = 0; m < AR_MC; m++) {
            double w[AR_P];
            for (int i = 0; i < AR_P; i++)
                w[i] = hist[AR_W - 1 - i] - mean;
            for (int h = 0; h < AR_H; h++) {
                double y = 0.0;
                for (int i = 0; i < AR_P; i++)
                    y += coef[i] * w[i];
                y += sigma * gauss(&rng);
                memmove(w + 1, w, (AR_P - 1) * sizeof(double));
                w[0] = y;
                if (y + mean > 27.0) {
                    exceed++;
                    break;
                }
            }
        }
        sink += (double)exceed / AR_MC + sigma;
        count(&g_compute_iters);
    }
    (void)sink;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Data unit: outlier removal + summarizing over a large history buffer     */
/* ------------------------------------------------------------------------ */
#define DATA_BATCH 65536   /* samples processed per batch (256 KB) */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void *data_unit(void *arg)
{
    uint64_t rng = 0xD1B54A32D192ED03ULL ^ ((uintptr_t)arg + 1) * 104729;
    size_t n = (size_t)cfg.ws_mb * 1024 * 1024 / sizeof(float);
    volatile double sink = 0.0;
    size_t pos = 0;

    if (n < DATA_BATCH)
        n = DATA_BATCH;
    n -= n % DATA_BATCH;

    float *ring = malloc(n * sizeof(float));
    if (!ring) {
        perror("data_unit: malloc");
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
        ring[i] = fast_temp(&rng);

    while (!stopping()) {
        /* fresh scratch buffers every batch, as a typical prototype would */
        float *work = malloc(DATA_BATCH * sizeof(float));
        float *dev  = malloc(DATA_BATCH * sizeof(float));
        if (!work || !dev) {
            free(work);
            free(dev);
            break;
        }
        memcpy(work, ring + pos, DATA_BATCH * sizeof(float));

        /* robust centre and spread: median and MAD */
        qsort(work, DATA_BATCH, sizeof(float), cmp_float);
        float med = work[DATA_BATCH / 2];
        for (size_t i = 0; i < DATA_BATCH; i++)
            dev[i] = fabsf(work[i] - med);
        qsort(dev, DATA_BATCH, sizeof(float), cmp_float);
        float mad = 1.4826f * dev[DATA_BATCH / 2];
        if (mad < 1e-6f)
            mad = 1e-6f;

        /* drop outliers, summarize the rest (Welford mean/variance) */
        double mn = 1e30, mx = -1e30, mean = 0.0, m2 = 0.0;
        long kept = 0;
        for (size_t i = 0; i < DATA_BATCH; i++) {
            float v = work[i];
            if (fabsf(v - med) > 3.5f * mad)
                continue;
            kept++;
            double d = v - mean;
            mean += d / kept;
            m2 += d * (v - mean);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        sink += mean + mn + mx + (kept > 1 ? m2 / (kept - 1) : 0.0);

        /* update the whole history: one touch per 64-byte cache line,
         * which streams the full working set through L1/L2/L3 and DRAM */
        for (size_t k = (pos / DATA_BATCH) % 16; k < n; k += 16)
            ring[k] = 0.999f * ring[k] + 0.001f * (float)mean;

        /* new raw samples arrive into the slot just processed */
        for (size_t i = 0; i < DATA_BATCH; i++)
            ring[pos + i] = fast_temp(&rng);

        free(work);
        free(dev);
        pos = (pos + DATA_BATCH) % n;
        count(&g_data_batches);
    }
    free(ring);
    (void)sink;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* I/O unit: high-rate sensor reads + logging                               */
/* ------------------------------------------------------------------------ */
static void *io_unit(void *arg)
{
    long id = (long)(uintptr_t)arg;
    uint64_t rng = 0xA0761D6478BD642FULL ^ (uint64_t)(id + 1);
    char path[512], rb[32], logbuf[64 * 1024];
    size_t used = 0;
    off_t written = 0;
    double t = 0.0;

    snprintf(path, sizeof path, "%s/critter_io_%d_%ld.log",
             cfg.io_dir, (int)getpid(), id);
    int lfd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (lfd < 0) {
        perror(path);
        return NULL;
    }
    int tfd = open(THERMAL_PATH, O_RDONLY);   /* real Pi 5 sensor */

    while (!stopping()) {
        long mc;
        ssize_t r = (tfd >= 0) ? pread(tfd, rb, sizeof rb - 1, 0) : -1;
        if (r > 0) {
            rb[r] = '\0';
            mc = strtol(rb, NULL, 10);
        } else {                               /* no sensor: synthesize */
            mc = (long)(synth_temp(&rng, t) * 1000.0);
            t += 0.001;
        }
        used += (size_t)snprintf(logbuf + used, sizeof logbuf - used,
                                 "%lld,%ld\n", (long long)now_ns(), mc);
        count(&g_io_reads);

        if (used > sizeof logbuf - 64) {
            if (write(lfd, logbuf, used) < 0) {
                perror("io_unit: write");
                break;
            }
            written += (off_t)used;
            used = 0;
            if (cfg.io_sync)
                fsync(lfd);
            if (written > ((off_t)64 << 20)) {     /* keep the file <= 64 MB */
                if (ftruncate(lfd, 0) != 0)
                    perror("io_unit: ftruncate");
                written = 0;
            }
        }
    }
    close(lfd);
    if (tfd >= 0)
        close(tfd);
    unlink(path);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Network unit: Critter-to-collector UDP summaries                         */
/* ------------------------------------------------------------------------ */
static void *net_unit(void *arg)
{
    long id = (long)(uintptr_t)arg;
    uint64_t rng = 0xE7037ED1A0B428DBULL ^ (uint64_t)(id + 1);
    struct sockaddr_in dst;
    char pkt[1024];
    unsigned long seq = 0;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("net_unit: socket");
        return NULL;
    }
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)cfg.net_port);
    if (inet_pton(AF_INET, cfg.net_target, &dst.sin_addr) != 1) {
        fprintf(stderr, "net_unit: bad -T address '%s'\n", cfg.net_target);
        close(s);
        return NULL;
    }

    while (!stopping()) {
        int len = snprintf(pkt, sizeof pkt,
                           "CRITTER room=%ld seq=%lu min=%.2f max=%.2f mean=%.2f ",
                           id, seq++, 20.0 + urand(&rng), 24.0 + urand(&rng),
                           22.0 + urand(&rng));
        if (len > 0 && len < (int)sizeof pkt)
            memset(pkt + len, '.', sizeof pkt - (size_t)len);
        if (sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst) > 0)
            count(&g_net_pkts);
    }
    close(s);
    return NULL;
}

/* local receiver so loopback traffic is actually consumed */
static void *net_sink(void *arg)
{
    struct sockaddr_in a;
    struct timeval tv = { 0, 100000 };
    char buf[2048];
    int one = 1;
    (void)arg;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return NULL;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)cfg.net_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("net_sink: bind");
        close(s);
        return NULL;
    }
    while (!stopping())
        (void)recv(s, buf, sizeof buf, 0);
    close(s);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Memory hog: emulate a board with less RAM                                */
/* ------------------------------------------------------------------------ */
static void *hog_unit(void *arg)
{
    size_t bytes = (size_t)cfg.hog_mb << 20;
    long pg = sysconf(_SC_PAGESIZE);   /* 16 KB on the Pi 5 kernel */
    (void)arg;

    char *p = malloc(bytes);
    if (!p) {
        perror("hog_unit: malloc");
        atomic_store(&g_hog_ready, 1);
        return NULL;
    }
    for (size_t i = 0; i < bytes; i += (size_t)pg)
        p[i] = 1;
    atomic_store(&g_hog_ready, 1);

    /* keep every page recently used so the kernel cannot quietly reclaim it */
    while (!stopping()) {
        for (size_t i = 0; i < bytes && !stopping(); i += (size_t)pg)
            p[i]++;
        usleep(10000);
    }
    free(p);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Thread management                                                        */
/* ------------------------------------------------------------------------ */
static void spawn(void *(*fn)(void *), long id, const cpu_set_t *mask)
{
    if (g_nthreads >= MAX_THREADS) {
        fprintf(stderr, "too many threads (max %d)\n", MAX_THREADS);
        exit(1);
    }
    int e = pthread_create(&g_threads[g_nthreads], NULL, fn, (void *)(uintptr_t)id);
    if (e) {
        fprintf(stderr, "pthread_create: %s\n", strerror(e));
        exit(1);
    }
    if (mask) {
        e = pthread_setaffinity_np(g_threads[g_nthreads], sizeof *mask, mask);
        if (e)
            fprintf(stderr, "load affinity: %s\n", strerror(e));
    }
    g_nthreads++;
}

/* Apply OS controls to the sampler (calling) thread. Fills a description. */
static void setup_sampler(char *desc, size_t len)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);

    if (cfg.sampler_cpu >= 0) {
        cpu_set_t s;
        CPU_ZERO(&s);
        CPU_SET(cfg.sampler_cpu, &s);
        int e = pthread_setaffinity_np(pthread_self(), sizeof s, &s);
        if (e)
            fprintf(stderr, "sampler affinity: %s\n", strerror(e));
    }

    if (cfg.policy == SCHED_OTHER) {
        if (cfg.prio_set && setpriority(PRIO_PROCESS, (id_t)tid, cfg.prio) != 0)
            fprintf(stderr, "setpriority(nice %d): %s (negative nice needs sudo)\n",
                    cfg.prio, strerror(errno));
    } else {
        struct sched_param sp = { .sched_priority = cfg.prio };
        int e = pthread_setschedparam(pthread_self(), cfg.policy, &sp);
        if (e)
            fprintf(stderr, "pthread_setschedparam: %s (real-time policy needs sudo)\n",
                    strerror(e));
    }

    if (cfg.timer_slack_ns >= 0) {
        /* note: 0 means "reset to default" to the kernel, so 1 is the minimum */
        if (prctl(PR_SET_TIMERSLACK, (unsigned long)cfg.timer_slack_ns, 0, 0, 0) != 0)
            perror("prctl(PR_SET_TIMERSLACK)");
    }

    if (cfg.do_mlock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("mlockall (needs sudo or a higher memlock ulimit)");

    /* report what the kernel actually gave us, not what we asked for */
    int pol;
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &pol, &sp);
    errno = 0;
    int nice_v = getpriority(PRIO_PROCESS, (id_t)tid);
    long slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    if (pol == SCHED_FIFO || pol == SCHED_RR)
        snprintf(desc, len, "%s/%d", pol == SCHED_FIFO ? "fifo" : "rr", sp.sched_priority);
    else
        snprintf(desc, len, "other/nice%d", nice_v);
    cfg.timer_slack_ns = slack;
}

static void prefault_stack(void)
{
    volatile char buf[256 * 1024];
    for (size_t i = 0; i < sizeof buf; i += 4096)
        buf[i] = 0;
}

/* ------------------------------------------------------------------------ */
/* Statistics                                                               */
/* ------------------------------------------------------------------------ */
typedef struct {
    double min, mean, med, p90, p99, p999, max, sd;
} stats_t;

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* v in ns -> stats in us */
static stats_t calc_stats(const int64_t *v, long n)
{
    stats_t s = { 0 };
    if (n <= 0)
        return s;
    int64_t *c = malloc((size_t)n * sizeof *c);
    if (!c)
        return s;
    memcpy(c, v, (size_t)n * sizeof *c);
    qsort(c, (size_t)n, sizeof *c, cmp_i64);

    double sum = 0.0, sq = 0.0;
    for (long i = 0; i < n; i++)
        sum += c[i] / 1000.0;
    s.mean = sum / n;
    for (long i = 0; i < n; i++) {
        double d = c[i] / 1000.0 - s.mean;
        sq += d * d;
    }
    s.sd = sqrt(sq / n);
#define PCT(p) (c[(long)((p) * (n - 1) + 0.5)] / 1000.0)
    s.min = c[0] / 1000.0;
    s.med = PCT(0.50);
    s.p90 = PCT(0.90);
    s.p99 = PCT(0.99);
    s.p999 = PCT(0.999);
    s.max = c[n - 1] / 1000.0;
#undef PCT
    free(c);
    return s;
}

/* ------------------------------------------------------------------------ */
/* CLI                                                                      */
/* ------------------------------------------------------------------------ */
static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Sampler (the periodic Critter temperature sample being measured):\n"
        "  -p us      sample period in microseconds (default 1000)\n"
        "  -n count   number of samples (default 10000)\n"
        "  -m mode    sleep method: usleep | nanosleep | abs (default usleep)\n"
        "  -r         read the real Pi thermal sensor every period\n"
        "  -D us      soft deadline on wake-up latency (default = period)\n"
        "  -x         printf every sample inside the timed loop (naive sleeploop style)\n"
        "  -Q         attribution mode: per sample, record runqueue wait during the sleep,\n"
        "             and (with -r) CPU / wait / blocked time and context switches of the read\n"
        "  -W ms      warm-up before measuring (default 1000)\n"
        "Critter units as background load (program types):\n"
        "  -c n       compute threads (HVAC prediction: AR model + Monte Carlo)\n"
        "  -d n       data threads (outlier removal + summary, memory bound)\n"
        "  -w MB      working set per data thread (default 32)\n"
        "  -i n       I/O threads (high-rate sensor read + log write)\n"
        "  -S         fsync the I/O log every 64 KB (forces real storage writes)\n"
        "  -f dir     directory for I/O log files (default .)\n"
        "  -N n       network threads (UDP summaries to other Critters)\n"
        "  -T ip      network target (default 127.0.0.1; a local sink is started)\n"
        "  -H MB      memory hog: hold this much RAM in use (emulate smaller RAM)\n"
        "OS / platform controls for the sampler:\n"
        "  -s policy  other | fifo | rr (default other)\n"
        "  -P n       nice value (other) or RT priority 1-99 (fifo/rr, default 50)\n"
        "  -a cpu     pin sampler to this CPU\n"
        "  -A list    pin load threads to CPUs, e.g. 0-2 or 0,1\n"
        "  -l         mlockall() so this process is never paged\n"
        "  -t ns      timer slack (Linux default 50000; 1 = minimum)\n"
        "Output:\n"
        "  -o file    per-sample CSV\n"
        "  -R file    append one summary row to this CSV\n"
        "  -L label   run label, e.g. type-compute4\n", p);
}

static void write_summary_row(const char *pol_desc, long online, int allowed,
                              const stats_t *L, const stats_t *I, const stats_t *Wk,
                              long misses, long overruns, double hz,
                              const struct rusage *ru, double t0, double t1,
                              long f0, long f1)
{
    FILE *f = fopen(cfg.summary_path, "a+");
    if (!f) {
        perror(cfg.summary_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fprintf(f,
            "label,period_us,sleep_mode,policy,timer_slack_ns,sampler_cpu,load_cpus,"
            "mlock,compute,data,io,io_sync,net,ws_mb,hog_mb,online_cpus,allowed_cpus,"
            "samples,lat_min_us,lat_mean_us,lat_median_us,lat_p90_us,lat_p99_us,"
            "lat_p999_us,lat_max_us,lat_sd_us,deadline_us,misses,overruns,achieved_hz,"
            "interval_sd_us,work_p99_us,work_max_us,nvcsw,nivcsw,minflt,majflt,"
            "temp_start_c,temp_end_c,freq_start_mhz,freq_end_mhz,"
            "compute_iters,data_batches,io_reads,net_pkts\n");

    char lc[64];
    snprintf(lc, sizeof lc, "%s", cfg.load_cpus[0] ? cfg.load_cpus : "any");
    for (char *c = lc; *c; c++)
        if (*c == ',')
            *c = ';';

    fprintf(f,
        "%s,%ld,%s,%s,%ld,%d,%s,%d,%d,%d,%d,%d,%d,%ld,%ld,%ld,%d,"
        "%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%ld,%ld,%ld,%.3f,"
        "%.3f,%.3f,%.3f,%ld,%ld,%ld,%ld,%.1f,%.1f,%ld,%ld,%lu,%lu,%lu,%lu\n",
        cfg.label, cfg.period_us, sleep_names[cfg.sleep_mode], pol_desc,
        cfg.timer_slack_ns, cfg.sampler_cpu, lc, cfg.do_mlock,
        cfg.n_compute, cfg.n_data, cfg.n_io, cfg.io_sync, cfg.n_net,
        cfg.ws_mb, cfg.hog_mb, online, allowed,
        cfg.nsamples, L->min, L->mean, L->med, L->p90, L->p99, L->p999, L->max, L->sd,
        cfg.deadline_us, misses, overruns, hz,
        I->sd, Wk->p99, Wk->max,
        ru->ru_nvcsw, ru->ru_nivcsw, ru->ru_minflt, ru->ru_majflt,
        t0, t1, f0, f1,
        atomic_load(&g_compute_iters), atomic_load(&g_data_batches),
        atomic_load(&g_io_reads), atomic_load(&g_net_pkts));
    fclose(f);
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "p:n:m:rD:xQW:c:d:w:i:Sf:N:T:H:s:P:a:A:lt:o:R:L:h")) != -1) {
        switch (opt) {
        case 'p': cfg.period_us = atol(optarg); break;
        case 'n': cfg.nsamples = atol(optarg); break;
        case 'm':
            if (!strcmp(optarg, "usleep"))         cfg.sleep_mode = SLEEP_USLEEP;
            else if (!strcmp(optarg, "nanosleep")) cfg.sleep_mode = SLEEP_NANOSLEEP;
            else if (!strcmp(optarg, "abs"))       cfg.sleep_mode = SLEEP_ABS;
            else { usage(argv[0]); return 1; }
            break;
        case 'r': cfg.read_sensor = 1; break;
        case 'D': cfg.deadline_us = atol(optarg); break;
        case 'x': cfg.live_print = 1; break;
        case 'Q': cfg.attrib = 1; break;
        case 'W': cfg.warmup_ms = atol(optarg); break;
        case 'c': cfg.n_compute = atoi(optarg); break;
        case 'd': cfg.n_data = atoi(optarg); break;
        case 'w': cfg.ws_mb = atol(optarg); break;
        case 'i': cfg.n_io = atoi(optarg); break;
        case 'S': cfg.io_sync = 1; break;
        case 'f': snprintf(cfg.io_dir, sizeof cfg.io_dir, "%s", optarg); break;
        case 'N': cfg.n_net = atoi(optarg); break;
        case 'T': snprintf(cfg.net_target, sizeof cfg.net_target, "%s", optarg); break;
        case 'H': cfg.hog_mb = atol(optarg); break;
        case 's':
            if (!strcmp(optarg, "other"))     cfg.policy = SCHED_OTHER;
            else if (!strcmp(optarg, "fifo")) cfg.policy = SCHED_FIFO;
            else if (!strcmp(optarg, "rr"))   cfg.policy = SCHED_RR;
            else { usage(argv[0]); return 1; }
            break;
        case 'P': cfg.prio = atoi(optarg); cfg.prio_set = 1; break;
        case 'a': cfg.sampler_cpu = atoi(optarg); break;
        case 'A': snprintf(cfg.load_cpus, sizeof cfg.load_cpus, "%s", optarg); break;
        case 'l': cfg.do_mlock = 1; break;
        case 't': cfg.timer_slack_ns = atol(optarg); break;
        case 'o': snprintf(cfg.csv_path, sizeof cfg.csv_path, "%s", optarg); break;
        case 'R': snprintf(cfg.summary_path, sizeof cfg.summary_path, "%s", optarg); break;
        case 'L': snprintf(cfg.label, sizeof cfg.label, "%s", optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (cfg.period_us <= 0 || cfg.nsamples <= 1) {
        fprintf(stderr, "period and sample count must be positive\n");
        return 1;
    }
    if (cfg.deadline_us < 0)
        cfg.deadline_us = cfg.period_us;
    if (cfg.policy != SCHED_OTHER && !cfg.prio_set)
        cfg.prio = 50;
    if (cfg.do_mlock && cfg.hog_mb > 0)
        fprintf(stderr, "warning: -l with -H also locks the hog's memory; OOM is likely\n");

    cpu_set_t allowed_set;
    sched_getaffinity(0, sizeof allowed_set, &allowed_set);
    int  allowed_cpus = CPU_COUNT(&allowed_set);   /* reflects taskset */
    long online_cpus  = sysconf(_SC_NPROCESSORS_ONLN);

    /* preallocate and touch sample storage so the loop never page-faults on it */
    sample_t *smp = calloc((size_t)cfg.nsamples, sizeof *smp);
    int64_t  *tmp = calloc((size_t)cfg.nsamples, sizeof *tmp);
    if (!smp || !tmp) {
        perror("calloc");
        return 1;
    }
    memset(smp, 0, (size_t)cfg.nsamples * sizeof *smp);
    memset(tmp, 0, (size_t)cfg.nsamples * sizeof *tmp);

    cpu_set_t load_mask, *lm = NULL;
    if (cfg.load_cpus[0]) {
        if (parse_cpu_list(cfg.load_cpus, &load_mask)) {
            fprintf(stderr, "bad -A cpu list '%s'\n", cfg.load_cpus);
            return 1;
        }
        lm = &load_mask;
    }

    /* start background Critter units (created before the sampler changes its
     * own policy, so they stay at normal priority) */
    if (cfg.hog_mb > 0) {
        spawn(hog_unit, 0, lm);
        while (!atomic_load(&g_hog_ready))
            usleep(1000);
    }
    for (int i = 0; i < cfg.n_compute; i++) spawn(compute_unit, i, lm);
    for (int i = 0; i < cfg.n_data; i++)    spawn(data_unit, i, lm);
    for (int i = 0; i < cfg.n_io; i++)      spawn(io_unit, i, lm);
    for (int i = 0; i < cfg.n_net; i++)     spawn(net_unit, i, lm);
    if (cfg.n_net > 0 && !strcmp(cfg.net_target, "127.0.0.1"))
        spawn(net_sink, 0, lm);

    char pol_desc[32];
    setup_sampler(pol_desc, sizeof pol_desc);
    prefault_stack();
    usleep((useconds_t)(cfg.warmup_ms * 1000));   /* let the loads reach steady state */

    int tfd = cfg.read_sensor ? open(THERMAL_PATH, O_RDONLY) : -1;
    if (cfg.read_sensor && tfd < 0)
        fprintf(stderr, "note: %s not available, -r will time a failed read\n", THERMAL_PATH);

    int sfd = -1;
    int64_t last_wait = 0;
    if (cfg.attrib) {
        sfd = open("/proc/thread-self/schedstat", O_RDONLY);
        if (sfd < 0 || read_wait_ns(sfd, &last_wait) != 0) {
            fprintf(stderr, "note: schedstat unavailable, -Q disabled\n");
            if (sfd >= 0) close(sfd);
            sfd = -1;
            cfg.attrib = 0;
        }
    }

    double temp0 = read_temp_c();
    long   freq0 = read_sysfs_long(FREQ_PATH);
    freq0 = freq0 < 0 ? -1 : freq0 / 1000;
    struct rusage ru0, ru1;
    getrusage(RUSAGE_THREAD, &ru0);

    /* -------------------------- timed loop -------------------------- */
    const int64_t period_ns = cfg.period_us * 1000;
    const int64_t deadline_ns = cfg.deadline_us * 1000;
    struct timespec next;
    long misses = 0, overruns = 0;
    char rb[32];

    clock_gettime(CLOCK_MONOTONIC, &next);
    ts_add_ns(&next, period_ns);
    int64_t prev_wake = now_ns();
    int64_t first_wake = 0, last_wake = 0;

    for (long i = 0; i < cfg.nsamples; i++) {
        int64_t before = now_ns(), intended;

        switch (cfg.sleep_mode) {
        case SLEEP_USLEEP:
            usleep((useconds_t)cfg.period_us);
            intended = before + period_ns;
            break;
        case SLEEP_NANOSLEEP: {
            struct timespec r = { period_ns / NS_PER_SEC, period_ns % NS_PER_SEC };
            while (nanosleep(&r, &r) == -1 && errno == EINTR)
                ;
            intended = before + period_ns;
            break;
        }
        default: /* SLEEP_ABS: fixed schedule, no drift */
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
                ;
            intended = ts_ns(&next);
            break;
        }

        int64_t wake = now_ns();
        int64_t work = 0;
        int32_t temp = 0;
        int64_t w_sleep = 0, rd_cpu = 0, rd_wait = 0;
        int32_t rd_v = 0, rd_iv = 0;
        if (cfg.attrib) {            /* wait accrued during sleep + wake-up */
            int64_t w1;
            if (read_wait_ns(sfd, &w1) == 0) {
                w_sleep = w1 - last_wait;
                last_wait = w1;
            }
        }
        if (cfg.read_sensor) {
            struct rusage a, b2;
            int64_t c0 = 0, t0 = wake;
            if (cfg.attrib) {
                getrusage(RUSAGE_THREAD, &a);
                c0 = thread_cpu_ns();
                t0 = now_ns();       /* time the read itself, not the bookkeeping */
            }
            ssize_t r = (tfd >= 0) ? pread(tfd, rb, sizeof rb - 1, 0) : -1;
            if (r > 0) {
                rb[r] = '\0';
                temp = (int32_t)strtol(rb, NULL, 10);
            }
            work = now_ns() - t0;
            if (cfg.attrib) {
                int64_t w2;
                rd_cpu = thread_cpu_ns() - c0;
                getrusage(RUSAGE_THREAD, &b2);
                rd_v = (int32_t)(b2.ru_nvcsw - a.ru_nvcsw);
                rd_iv = (int32_t)(b2.ru_nivcsw - a.ru_nivcsw);
                if (read_wait_ns(sfd, &w2) == 0) {
                    rd_wait = w2 - last_wait;
                    last_wait = w2;
                }
            }
        }

        smp[i].lat_ns = wake - intended;
        smp[i].work_ns = work;
        smp[i].interval_ns = wake - prev_wake;
        smp[i].temp_mc = temp;
        smp[i].sleep_wait_ns = w_sleep;
        smp[i].rd_cpu_ns = rd_cpu;
        smp[i].rd_wait_ns = rd_wait;
        smp[i].rd_vcsw = rd_v;
        smp[i].rd_ivcsw = rd_iv;
        if (smp[i].lat_ns > deadline_ns)
            misses++;
        if (i == 0)
            first_wake = wake;
        last_wake = wake;
        prev_wake = wake;

        if (cfg.live_print)   /* deliberately inside the loop: observer effect */
            printf("%ld,%lld,%lld\n", i, (long long)smp[i].lat_ns, (long long)work);

        if (cfg.sleep_mode == SLEEP_ABS) {
            ts_add_ns(&next, period_ns);
            int64_t t = now_ns();
            while (ts_ns(&next) <= t) {      /* fell behind: skip missed slots */
                ts_add_ns(&next, period_ns);
                overruns++;
            }
        }
    }
    /* ---------------------------------------------------------------- */

    getrusage(RUSAGE_THREAD, &ru1);
    double temp1 = read_temp_c();
    long   freq1 = read_sysfs_long(FREQ_PATH);
    freq1 = freq1 < 0 ? -1 : freq1 / 1000;

    atomic_store(&g_stop, 1);
    for (int i = 0; i < g_nthreads; i++)
        pthread_join(g_threads[i], NULL);
    if (tfd >= 0)
        close(tfd);
    if (cfg.live_print)
        fflush(stdout);

    /* --------------------------- results ---------------------------- */
    for (long i = 0; i < cfg.nsamples; i++)
        tmp[i] = smp[i].lat_ns;
    stats_t L = calc_stats(tmp, cfg.nsamples);

    for (long i = 1; i < cfg.nsamples; i++)
        tmp[i - 1] = smp[i].interval_ns - period_ns;
    stats_t I = calc_stats(tmp, cfg.nsamples - 1);

    stats_t Wk = { 0 };
    if (cfg.read_sensor) {
        for (long i = 0; i < cfg.nsamples; i++)
            tmp[i] = smp[i].work_ns;
        Wk = calc_stats(tmp, cfg.nsamples);
    }

    double span_s = (last_wake - first_wake) / 1e9;
    double hz = span_s > 0 ? (cfg.nsamples - 1) / span_s : 0.0;

    struct rusage d;
    memset(&d, 0, sizeof d);
    d.ru_nvcsw = ru1.ru_nvcsw - ru0.ru_nvcsw;
    d.ru_nivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;
    d.ru_minflt = ru1.ru_minflt - ru0.ru_minflt;
    d.ru_majflt = ru1.ru_majflt - ru0.ru_majflt;

    fprintf(stderr,
        "\n=== Critter predictability run: %s ===\n"
        "period %ld us, %ld samples, sleep=%s, sampler %s, slack %ld ns, cpu %d, mlock %d\n"
        "load: compute %d, data %d (%ld MB), io %d%s, net %d, hog %ld MB, cpus %d/%ld\n"
        "wake latency (us): min %.1f  median %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f  sd %.1f\n"
        "soft deadline %ld us: %ld misses (%.3f%%), %ld skipped slots\n"
        "achieved rate %.1f Hz (target %.1f Hz), interval jitter sd %.1f us\n",
        cfg.label, cfg.period_us, cfg.nsamples, sleep_names[cfg.sleep_mode],
        pol_desc, cfg.timer_slack_ns, cfg.sampler_cpu, cfg.do_mlock,
        cfg.n_compute, cfg.n_data, cfg.ws_mb, cfg.n_io, cfg.io_sync ? " (fsync)" : "",
        cfg.n_net, cfg.hog_mb, allowed_cpus, online_cpus,
        L.min, L.med, L.p90, L.p99, L.p999, L.max, L.sd,
        cfg.deadline_us, misses, 100.0 * misses / cfg.nsamples, overruns,
        hz, 1e6 / cfg.period_us, I.sd);
    if (cfg.read_sensor)
        fprintf(stderr, "sensor read time (us): median %.1f  p99 %.1f  max %.1f\n",
                Wk.med, Wk.p99, Wk.max);
    fprintf(stderr,
        "sampler thread: %ld voluntary / %ld involuntary ctx switches, %ld minor / %ld major faults\n"
        "cpu temp %.1f -> %.1f C, cpu0 freq %ld -> %ld MHz\n"
        "load work done: compute %lu, data %lu, io %lu, net %lu\n",
        d.ru_nvcsw, d.ru_nivcsw, d.ru_minflt, d.ru_majflt,
        temp0, temp1, freq0, freq1,
        atomic_load(&g_compute_iters), atomic_load(&g_data_batches),
        atomic_load(&g_io_reads), atomic_load(&g_net_pkts));

    if (cfg.csv_path[0]) {
        FILE *f = fopen(cfg.csv_path, "w");
        if (f) {
            fprintf(f, "i,lat_ns,work_ns,interval_ns,temp_mC%s\n",
                    cfg.attrib ? ",sleep_wait_ns,rd_cpu_ns,rd_wait_ns,rd_vcsw,rd_ivcsw" : "");
            for (long i = 0; i < cfg.nsamples; i++) {
                fprintf(f, "%ld,%lld,%lld,%lld,%d", i,
                        (long long)smp[i].lat_ns, (long long)smp[i].work_ns,
                        (long long)smp[i].interval_ns, smp[i].temp_mc);
                if (cfg.attrib)
                    fprintf(f, ",%lld,%lld,%lld,%d,%d", (long long)smp[i].sleep_wait_ns,
                            (long long)smp[i].rd_cpu_ns, (long long)smp[i].rd_wait_ns,
                            smp[i].rd_vcsw, smp[i].rd_ivcsw);
                fputc('\n', f);
            }
            fclose(f);
        } else {
            perror(cfg.csv_path);
        }
    }
    if (cfg.summary_path[0])
        write_summary_row(pol_desc, online_cpus, allowed_cpus, &L, &I, &Wk,
                          misses, overruns, hz, &d, temp0, temp1, freq0, freq1);

    if (sfd >= 0)
        close(sfd);
    free(smp);
    free(tmp);
    return 0;
}
