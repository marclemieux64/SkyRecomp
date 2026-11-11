// What the Quest build leaves out for now (android/engine/CMakeLists.txt): the desktop start screen and FFmpeg movies.
#include <stdbool.h>
#include <stdio.h>

// ---- runtime/ui/launcher.mm: game setup happens on the PC (tools/quest), so the game boots directly.
void launcher_run(const char *root) { (void)root; }
void launcher_play_seed(const char *seed) { (void)seed; }

// ---- runtime/hle/movie.c
bool movie_playing(void) { return false; }

// ---- runtime/dispatch.c's SKYRECOMP_TRACE_STACK debug trace captures the host stack with this Windows call.
#include <unwind.h>
typedef struct { void **pcs; unsigned long skip, n, max; } Capture;
static _Unwind_Reason_Code capture_frame(struct _Unwind_Context *ctx, void *arg) {
    Capture *c = arg;
    void *pc = (void *)_Unwind_GetIP(ctx);
    if (!pc) return _URC_END_OF_STACK;
    if (c->skip) { c->skip--; return _URC_NO_REASON; }
    if (c->n < c->max) c->pcs[c->n++] = pc;
    return c->n < c->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
unsigned short RtlCaptureStackBackTrace(unsigned long skip, unsigned long max, void **frames, unsigned long *hash) {
    (void)hash;
    Capture c = {frames, skip + 1, 0, max};
    _Unwind_Backtrace(capture_frame, &c);
    return (unsigned short)c.n;
}
