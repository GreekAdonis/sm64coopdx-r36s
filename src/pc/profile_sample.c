#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "profile_sample.h"

#ifdef PROFILE_BUILD

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <link.h>
#include <pthread.h>

// Where the sampled program counter lives in the signal's ucontext.
#if defined(__aarch64__)
    #define PROFILE_SAMPLE_PC(_uc) ((unsigned long)((ucontext_t *)(_uc))->uc_mcontext.pc)
#elif defined(__arm__)
    #define PROFILE_SAMPLE_PC(_uc) ((unsigned long)((ucontext_t *)(_uc))->uc_mcontext.arm_pc)
#elif defined(__x86_64__)
    #define PROFILE_SAMPLE_PC(_uc) ((unsigned long)((ucontext_t *)(_uc))->uc_mcontext.gregs[REG_RIP])
#elif defined(__i386__)
    #define PROFILE_SAMPLE_PC(_uc) ((unsigned long)((ucontext_t *)(_uc))->uc_mcontext.gregs[REG_EIP])
#else
    #define PROFILE_SAMPLE_PC(_uc) 0UL
#endif

#define PROFILE_TABLE_BITS  15
#define PROFILE_TABLE_SIZE  (1 << PROFILE_TABLE_BITS)
#define PROFILE_TABLE_MASK  (PROFILE_TABLE_SIZE - 1)
#define PROFILE_MAX_PROBES  32
#define PROFILE_DEFAULT_HZ  200

struct ProfileSampleSlot {
    unsigned long pc;
    unsigned int count;
};

// Written from the signal handler, so: preallocated, no locks, no libc.
static struct ProfileSampleSlot sSlots[PROFILE_TABLE_SIZE];
static volatile unsigned int sTaken = 0;      // samples recorded
static volatile unsigned int sOtherThread = 0; // samples that hit another thread
static volatile unsigned int sOverflow = 0;    // samples dropped, table full
static volatile unsigned int sNoPc = 0;        // samples with an unusable pc

static pid_t sMainTid = 0;
static timer_t sTimer;
static bool sTimerCreated = false;
static bool sRunning = false;
static int sHz = PROFILE_DEFAULT_HZ;

// Window state. Only the main thread touches these; the handler never does.
static FILE  *sFile = NULL;
static double sWindowSeconds = 0.0;
static double sWindowStart = 0.0;
static unsigned int sWindowIndex = 0;
static unsigned long sTotalWritten = 0;

static void profile_sample_handler(int sig, siginfo_t *info, void *uc) {
    (void)sig; (void)info;
    // Only the game thread is interesting. The timer counts this thread's CPU
    // time, but the kernel may still deliver the signal to whichever thread has
    // it unblocked, so drop anything that arrives elsewhere rather than mixing
    // audio/network stacks into the histogram.
    if (syscall(SYS_gettid) != sMainTid) { sOtherThread++; return; }

    unsigned long pc = PROFILE_SAMPLE_PC(uc);
    if (pc == 0) { sNoPc++; return; }

    size_t idx = (size_t)((pc >> 2) * 2654435761u) & PROFILE_TABLE_MASK;
    for (int i = 0; i < PROFILE_MAX_PROBES; i++) {
        struct ProfileSampleSlot *slot = &sSlots[idx];
        if (slot->pc == pc) { slot->count++; sTaken++; return; }
        if (slot->pc == 0)  { slot->pc = pc; slot->count = 1; sTaken++; return; }
        idx = (idx + 1) & PROFILE_TABLE_MASK;
    }
    sOverflow++;
}

struct ProfileModuleWriter {
    FILE *f;
    int index;
};

static int profile_sample_write_module(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct ProfileModuleWriter *w = (struct ProfileModuleWriter *)data;
    const char *name = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "(main)";

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) { continue; }
        unsigned long start = (unsigned long)(info->dlpi_addr + ph->p_vaddr);
        fprintf(w->f, "map 0x%lx 0x%lx 0x%lx %s\n",
                start, start + (unsigned long)ph->p_memsz,
                (unsigned long)info->dlpi_addr, name);
    }
    w->index++;
    return 0;
}

// Writes everything currently in the table as one window and empties it.
//
// The handler runs on this same thread, so it can interrupt the walk below and
// leave a window half-counted. Blocking SIGPROF for the duration is one syscall
// pair per window -- once every 30 seconds -- and costs at most the single
// sample that would have landed during the flush, which the kernel delivers
// afterwards anyway.
static void profile_sample_flush_window(double nowSeconds) {
    if (!sFile) { return; }

    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &block, &prev);

    unsigned int taken = sTaken, other = sOtherThread, full = sOverflow, nopc = sNoPc;
    sTaken = sOtherThread = sOverflow = sNoPc = 0;

    fprintf(sFile, "window %u %.3f %.3f %u %u %u %u\n",
            sWindowIndex, sWindowStart, nowSeconds, taken, other, full, nopc);

    for (size_t i = 0; i < PROFILE_TABLE_SIZE; i++) {
        if (sSlots[i].pc == 0) { continue; }
        fprintf(sFile, "0x%lx %u\n", sSlots[i].pc, sSlots[i].count);
        sSlots[i].pc = 0;
        sSlots[i].count = 0;
    }

    pthread_sigmask(SIG_SETMASK, &prev, NULL);

    // Flush to the OS so a kill -9 keeps every completed window.
    fflush(sFile);

    sTotalWritten += taken;
    sWindowIndex++;
    sWindowStart = nowSeconds;
}

void profile_sample_tick(double nowSeconds) {
    if (!sRunning || !sFile) { return; }
    if (sWindowSeconds <= 0.0) { return; }
    if (nowSeconds - sWindowStart < sWindowSeconds) { return; }
    profile_sample_flush_window(nowSeconds);
}

void profile_sample_init(const char *path, double windowSeconds) {
    if (sRunning) { return; }

    const char *enabled = getenv("SM64_PROFILE_SAMPLE");
    if (enabled && (!strcmp(enabled, "0") || !strcmp(enabled, "off"))) {
        printf("[profile] sampling disabled by SM64_PROFILE_SAMPLE\n");
        return;
    }
#if !defined(__aarch64__) && !defined(__arm__) && !defined(__x86_64__) && !defined(__i386__)
    printf("[profile] sampling unsupported on this architecture\n");
    return;
#endif

    const char *hz = getenv("SM64_PROFILE_SAMPLE_HZ");
    if (hz && atoi(hz) > 0) { sHz = atoi(hz); }
    if (sHz > 1000) { sHz = 1000; }

    sWindowSeconds = windowSeconds;

    sMainTid = (pid_t)syscall(SYS_gettid);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = profile_sample_handler;
    // SA_RESTART so the interrupted socket/poll calls in the frame loop resume
    // instead of surfacing EINTR to code that never expected it.
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) {
        printf("[profile] could not install SIGPROF handler, sampling disabled\n");
        return;
    }

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_signo = SIGPROF;
#if defined(SIGEV_THREAD_ID) && defined(sigev_notify_thread_id)
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_notify_thread_id = sMainTid;
#else
    sev.sigev_notify = SIGEV_SIGNAL;
#endif

    long periodNs = 1000000000L / sHz;
    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = periodNs;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = periodNs;

    // CLOCK_THREAD_CPUTIME_ID: sample the game thread's own CPU time, so the
    // busy-wait/sleep of the frame cap does not dominate the histogram and the
    // sample count is proportional to real work done.
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &sTimer) == 0 &&
        timer_settime(sTimer, 0, &its, NULL) == 0) {
        sTimerCreated = true;
        sRunning = true;
    } else {
        // Fall back to the process CPU-time interval timer.
        struct itimerval itv;
        itv.it_value.tv_sec = 0;
        itv.it_value.tv_usec = (suseconds_t)(periodNs / 1000);
        itv.it_interval = itv.it_value;
        if (setitimer(ITIMER_PROF, &itv, NULL) == 0) {
            sRunning = true;
        }
    }

    if (!sRunning) {
        printf("[profile] could not start sampling timer\n");
        return;
    }

    // Open now rather than at dump time: windows are appended as the session
    // runs, so a process that is killed instead of exiting still leaves a usable
    // file. The module map has to go in up front for the same reason -- it is
    // what turns a pc back into a symbol, and it is fixed once the process is
    // loaded (no dlopen happens after this point).
    sFile = fopen(path, "w");
    if (!sFile) {
        printf("[profile] could not open '%s' for samples\n", path);
        return;
    }

    fprintf(sFile, "# sm64coopdx sampled profile\n");
    fprintf(sFile, "hz %d\n", sHz);
    fprintf(sFile, "window_seconds %.3f\n", sWindowSeconds);

    struct ProfileModuleWriter writer = { sFile, 0 };
    dl_iterate_phdr(profile_sample_write_module, &writer);
    fflush(sFile);

    if (sWindowSeconds > 0.0) {
        printf("[profile] sampling main thread at %d Hz, %.0fs windows\n", sHz, sWindowSeconds);
    } else {
        printf("[profile] sampling main thread at %d Hz, single window\n", sHz);
    }
}

void profile_sample_dump(double nowSeconds) {
    if (!sRunning) { return; }

    // stop sampling before walking the table
    if (sTimerCreated) {
        timer_delete(sTimer);
        sTimerCreated = false;
    } else {
        struct itimerval off;
        memset(&off, 0, sizeof(off));
        setitimer(ITIMER_PROF, &off, NULL);
    }
    signal(SIGPROF, SIG_IGN);
    sRunning = false;

    if (!sFile) { return; }

    // The last window is whatever has accumulated since the previous boundary,
    // however short. Emitting it unconditionally keeps the invariant the report
    // tool relies on: every sample taken belongs to exactly one window.
    profile_sample_flush_window(nowSeconds);

    fclose(sFile);
    sFile = NULL;
    printf("[profile] wrote %lu samples in %u windows\n", sTotalWritten, sWindowIndex);
}

#endif
