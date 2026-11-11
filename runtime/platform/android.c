// Android / Meta Quest host layer (runtime/platform.h), from the Linux one. The process is an Activity: it cannot
// re-execute itself, has no terminal (the log goes to logcat, tag SkyRecomp) and keeps its data in the app's
// external files folder (/sdcard/Android/data/<package>/files), where the PC builder copies the game files.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <unwind.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && p == (void *)(uintptr_t)lo) return true;
    if (p != MAP_FAILED) munmap(p, hi - lo);  // (kernels before 4.17: just a hint)
    // An app's layout comes from the system's zygote process and only changes when the headset restarts. This runs
    // while the library loads, before the log is set up: say what is in the way straight to logcat.
    __android_log_print(ANDROID_LOG_ERROR, "SkyRecomp", "guest address range %llx-%llx is not free (%s); in the way:",
                        (unsigned long long)lo, (unsigned long long)hi, strerror(errno));
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    int shown = 0;
    while (maps && fgets(line, sizeof line, maps)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) == 2 && a < hi && b > lo && shown++ < 40)
            __android_log_print(ANDROID_LOG_ERROR, "SkyRecomp", "  %s", line);
    }
    if (maps) fclose(maps);
    __android_log_print(ANDROID_LOG_ERROR, "SkyRecomp", "restart the headset and try again");
    return false;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// stdout / stderr go to logcat, line by line (adb logcat -s SkyRecomp), and to skyrecomp.log in the data folder.
static char g_log_path[1024];
static int g_log_pipe[2];
static FILE *g_log_file;

static void *log_pump(void *arg) {
    (void)arg;
    char line[2048];
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(g_log_pipe[0], &c, 1);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        if (c != '\n' && n < sizeof line - 1) { line[n++] = c; continue; }
        line[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, "SkyRecomp", line);
        if (g_log_file) { fputs(line, g_log_file); fputc('\n', g_log_file); fflush(g_log_file); }
        n = 0;
        if (c != '\n') line[n++] = c;
    }
    return NULL;
}

static void log_to_logcat(const char *data_root) {
    if (data_root) {
        snprintf(g_log_path, sizeof g_log_path, "%s/skyrecomp.log", data_root);
        g_log_file = fopen(g_log_path, "w");
        if (!g_log_file) g_log_path[0] = 0;
    }
    if (pipe(g_log_pipe) != 0) return;
    dup2(g_log_pipe[1], STDOUT_FILENO);
    dup2(g_log_pipe[1], STDERR_FILENO);
    pthread_t t;
    if (pthread_create(&t, NULL, log_pump, NULL) == 0) pthread_detach(t);
}

void plat_init(void) {
    // SDL's activity has started us (SDL_main): its JNI helpers are ready.
    const char *data = SDL_GetAndroidExternalStoragePath();
    log_to_logcat(data);
    if (data && *data) setenv("LARECOMP_DATA_ROOT", data, 0);
    else __android_log_write(ANDROID_LOG_ERROR, "SkyRecomp", "no external files folder");
    // The game code is packaged in the APK next to this library (useLegacyPackaging: extracted to nativeLibraryDir).
    char lib[1100];
    if (plat_exe_path(lib, sizeof lib)) {
        char *s = strrchr(lib, '/');
        if (s) { snprintf(s + 1, sizeof lib - (size_t)(s + 1 - lib), "libskygame.so"); setenv("LARECOMP_GAME_LIB", lib, 0); }
    }
    setenv("LARECOMP_NO_LAUNCHER", "1", 0);      // no desktop start screen: boot the game
    setenv("LARECOMP_NO_UPDATE_CHECK", "1", 0);
    setenv("SKYRECOMP_VR", "1", 0);              // a headset: always VR
}

// "The executable": this library (libmain.so) in the app's native library folder.
bool plat_exe_path(char *buf, size_t size) {
    Dl_info di = {0};
    if (!dladdr((void *)plat_exe_path, &di) || !di.dli_fname || di.dli_fname[0] != '/') return false;
    return (size_t)snprintf(buf, size, "%s", di.dli_fname) < size;
}

// An Activity cannot replace its process: callers treat the return as failure and carry on in place.
void plat_exec(char *const argv[]) { (void)argv; }

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe skyrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "Sky Recomp stopped because of an error.\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sky Recomp", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

typedef struct { void **pcs; int n, max; } UnwindState;
static _Unwind_Reason_Code unwind_frame(struct _Unwind_Context *ctx, void *arg) {
    UnwindState *s = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && s->n < s->max) s->pcs[s->n++] = (void *)pc;
    return s->n < s->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void plat_print_backtrace(void) {
    void *bt[64];
    UnwindState state = {bt, 0, 64};
    _Unwind_Backtrace(unwind_frame, &state);
    int n = state.n;
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (skyrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown (as on Linux).
void plat_terminate(void) { _exit(0); }
