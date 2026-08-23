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

void profile_sample_init(void) {
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

    if (sRunning) {
        printf("[profile] sampling main thread at %d Hz\n", sHz);
    } else {
        printf("[profile] could not start sampling timer\n");
    }
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

void profile_sample_dump(const char *path) {
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

    FILE *f = fopen(path, "w");
    if (!f) {
        printf("[profile] could not write samples to '%s'\n", path);
        return;
    }

    fprintf(f, "# sm64coopdx sampled profile\n");
    fprintf(f, "hz %d\n", sHz);
    fprintf(f, "samples %u\n", sTaken);
    fprintf(f, "dropped_other_thread %u\n", sOtherThread);
    fprintf(f, "dropped_table_full %u\n", sOverflow);
    fprintf(f, "dropped_no_pc %u\n", sNoPc);

    // module map, so a pc in libmali/libSDL2 can be told apart from game code
    struct ProfileModuleWriter writer = { f, 0 };
    dl_iterate_phdr(profile_sample_write_module, &writer);

    for (size_t i = 0; i < PROFILE_TABLE_SIZE; i++) {
        if (sSlots[i].pc == 0) { continue; }
        fprintf(f, "0x%lx %u\n", sSlots[i].pc, sSlots[i].count);
    }

    fclose(f);
    printf("[profile] wrote %u samples to '%s'\n", sTaken, path);
}

#endif
