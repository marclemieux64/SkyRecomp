// Function-entry profiler. LARECOMP_FNPROF=<from>:<to> counts guest function entries (every generated
// function calls FN_ENTER) between those presented frames, then writes build/fnprof.txt (addr count, most
// called first). Diffing two runs shows which game functions a change brings in; counts also show hot code.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "hle.h"
#include "platform.h"

volatile int g_fnprof;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; uint64_t seq; } slots[NSLOT];
static _Atomic uint64_t g_seq;  // (the order functions were first entered in the window)

// LARECOMP_FNPROF_BT=<hex addr>[,...]: also print the host backtrace (guest functions are f_<addr>) the first time
// each listed function is entered inside the window - who calls it
static uint64_t bt_addrs[256];
static int bt_n = -1;
static _Atomic uint8_t bt_done[256];

uint64_t g_bt_first_x0;

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n < 0) {
        bt_n = 0;
        const char *e = getenv("LARECOMP_FNPROF_BT");
        for (const char *p = e; p && *p && bt_n < 256;) {
            bt_addrs[bt_n++] = strtoull(p, (char **)&p, 16);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && !atomic_exchange(&bt_done[i], 1)) {
            if (i == 0) g_bt_first_x0 = x0;
            uint64_t vt = x0 >= (MAIN_BASE + 0x0ull) && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t v100 = vt >= (MAIN_BASE + 0x0ull) && vt < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(vt + 0x100) : 0;
            uint64_t p8 = x0 >= (MAIN_BASE + 0x0ull) && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            Cpu *kernel_current_cpu(void);
            Cpu *g_fnprof_cpu = kernel_current_cpu();  // (this thread's registers: the call's x1, x2)
            const uint64_t x19 = g_fnprof_cpu ? g_fnprof_cpu->x[19] : 0;
            const bool x19_ok = x19 >= (MAIN_BASE + 0x0ull) && x19 < 0x7800000000ull;
            fprintf(stderr, "[fnprof-bt] %llx entered from guest lr %llx x0 %llx x1 %llx x2 %llx (vtable %llx, +0x100 %llx, [x0+8] %llx) "
                    "x19 %llx (vtable %llx, +0x148 %.1f %.1f %.1f):\n",
                    (unsigned long long)a, (unsigned long long)lr, (unsigned long long)x0,
                    (unsigned long long)(g_fnprof_cpu ? g_fnprof_cpu->x[1] : 0), (unsigned long long)(g_fnprof_cpu ? g_fnprof_cpu->x[2] : 0),
                    (unsigned long long)vt, (unsigned long long)v100, (unsigned long long)p8, (unsigned long long)x19,
                    (unsigned long long)(x19_ok ? *(uint64_t *)(uintptr_t)x19 : 0), x19_ok ? *(float *)(uintptr_t)(x19 + 0x148) : 0.0,
                    x19_ok ? *(float *)(uintptr_t)(x19 + 0x14c) : 0.0, x19_ok ? *(float *)(uintptr_t)(x19 + 0x150) : 0.0);
#if defined(_WIN32)
            {   // the chain as guest functions (f_<address>)
                extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void **, unsigned long *);
                const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);
                void *frames[64];
                const unsigned short k = RtlCaptureStackBackTrace(1, 64, frames, NULL);
                char line[4096];
                int len = snprintf(line, sizeof line, "[fnprof-bt]   chain:");
                for (unsigned short j = 0; j < k && len < (int)sizeof line - 40; j++) {
                    uint64_t off;
                    const char *s = rt_host_fn_name((uint64_t)(uintptr_t)frames[j], &off);
                    if (s) len += snprintf(line + len, sizeof line - len, " %s", s);
                }
                fprintf(stderr, "%s\n", line);
            }
#else
            plat_print_backtrace();  // (execinfo is macOS / glibc only)
#endif
        }
}

// LARECOMP_FNPROF_WATCH=<hex address>: inside the window, the first function entered after the u32 there changes -
// logged with its caller and chain (the writer is the function before it, or its caller)
volatile uint64_t g_fnprof_watch;  // (set at run time: watch this address from now on)
static void watch_check(uint64_t a, uint64_t lr) {
    static uint64_t addr = 1;
    static uint32_t last;
    if (g_fnprof_watch) { addr = g_fnprof_watch; g_fnprof_watch = 0; last = *(volatile uint32_t *)(uintptr_t)addr; }
    
    static int done;
    if (addr == 1) {
        const char *e = getenv("LARECOMP_FNPROF_WATCH");
        addr = e ? strtoull(e, NULL, 16) : 0;
        if (addr) last = *(volatile uint32_t *)(uintptr_t)addr;
    }
    static uint64_t prev_a, prev_lr;
    if (addr && done < 4) {
        const uint32_t v = *(volatile uint32_t *)(uintptr_t)addr;
        if (v != last) {
            done++;
            fprintf(stderr, "[fnprof-watch] %llx: %x -> %x before entering %llx (lr %llx); previous entry %llx (lr %llx)\n",
                    (unsigned long long)addr, last, v, (unsigned long long)a, (unsigned long long)lr,
                    (unsigned long long)prev_a, (unsigned long long)prev_lr);
            last = v;
#if defined(_WIN32)
            extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void **, unsigned long *);
            const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);
            void *frames[64];
            const unsigned short k = RtlCaptureStackBackTrace(1, 64, frames, NULL);
            char line[4096];
            int len = snprintf(line, sizeof line, "[fnprof-watch]   chain:");
            for (unsigned short j = 0; j < k && len < (int)sizeof line - 40; j++) {
                uint64_t off;
                const char *s = rt_host_fn_name((uint64_t)(uintptr_t)frames[j], &off);
                if (s) len += snprintf(line + len, sizeof line - len, " %s", s);
            }
            fprintf(stderr, "%s\n", line);
#endif
        }
    }
    prev_a = a, prev_lr = lr;
}

// (door) while set, Link's own wall / ground check (f_7000facfe4 called from his update f_7000ab7cf4) is skipped:
// bit 0 of its +100 flags, set on entry. The object is kept so the door can clear the bit again.
volatile int g_door_nowall;
volatile uint64_t g_door_nowall_obj;

void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (g_door_nowall && a == (MAIN_BASE + 0xfacfe4ull) && lr >= (MAIN_BASE + 0xab7cf4ull) && lr < (MAIN_BASE + 0xad99e4ull)) {
        // (a count of his checks to skip: the last one runs again, with the bit cleared)
        static uint32_t bit;  // (SKYRECOMP_DOOR_NOWALL_BIT: which flag - test)
        if (!bit) bit = getenv("SKYRECOMP_DOOR_NOWALL_MASK") ? (uint32_t)strtoul(getenv("SKYRECOMP_DOOR_NOWALL_MASK"), NULL, 16) : 1u;
        if (--g_door_nowall > 0) *(volatile uint32_t *)(uintptr_t)(x0 + 100) |= bit;
        else {
            *(volatile uint32_t *)(uintptr_t)(x0 + 100) &= ~bit;
            if (!getenv("LARECOMP_FNPROF")) g_fnprof = 0;  // (the entry hook done)
        }
        g_door_nowall_obj = x0;
    }
    {   // LARECOMP_FNPROF_ARG=<hex>[,...]: log functions entered with one of these values in x0..x3 (a few per value)
        static uint64_t vals[16];
        static int nv = -1, said[16];
        if (nv < 0) {
            nv = 0;
            for (const char *p = getenv("LARECOMP_FNPROF_ARG"); p && *p && nv < 16;) {
                vals[nv++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        if (nv) {
            Cpu *kernel_current_cpu(void);
            Cpu *cc = kernel_current_cpu();
            for (int r = 0; cc && r < 4; r++)
                for (int i = 0; i < nv; i++)
                    if (cc->x[r] == vals[i] && said[i] < 12) {
                        said[i]++;
                        fprintf(stderr, "[fnprof-arg] %llx entered with x%d = %llx (lr %llx, x0 %llx x1 %llx x2 %llx x3 %llx)\n",
                                (unsigned long long)a, r, (unsigned long long)vals[i], (unsigned long long)lr,
                                (unsigned long long)cc->x[0], (unsigned long long)cc->x[1], (unsigned long long)cc->x[2],
                                (unsigned long long)cc->x[3]);
                    }
        }
    }
    watch_check(a, lr);
    if (bt_n) bt_check(a, lr, x0);
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) {
            slots[h].seq = atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
        if (cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
}

static int cmp(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

void fnprof_frame(uint64_t frame) {
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_FNPROF");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }
    if (from < 0) return;
    if ((long)frame == from) { g_fnprof = 1; LOG("[fnprof] on at frame %llu\n", (unsigned long long)frame); }
    if ((long)frame != to) return;
    g_fnprof = 0;
    uint64_t (*rows)[3] = malloc(sizeof(uint64_t[3]) * NSLOT);
    int n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; rows[n][2] = slots[i].seq; n++; }
    qsort(rows, n, sizeof rows[0], cmp);
    const char *out = getenv("LARECOMP_FNPROF_OUT") ? getenv("LARECOMP_FNPROF_OUT") : "build/fnprof.txt";  // (parallel runs)
    FILE *f = fopen(out, "w");
    for (int i = 0; f && i < n; i++)  // (address, entries, order of its first entry)
        fprintf(f, "%llx %llu %llu\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1],
                (unsigned long long)rows[i][2]);
    if (f) fclose(f);
    free(rows);
    LOG("[fnprof] %d functions, written %s (frames %ld-%ld)\n", n, out, from, to);
    from = -1;
}

// LARECOMP_FNPROF=0:<to>: profile from process start (threads the game starts before the first frame)
__attribute__((constructor)) static void fnprof_early(void) {
    const char *e = getenv("LARECOMP_FNPROF");
    if (e && e[0] == '0' && e[1] == ':') g_fnprof = 1;
}
