/* Runtime tree-lang: output, penghentian program, region data array tumbuh, serialisasi,
 * serta thread & queue bertipe.
 * Dikompilasi oleh clang bersama .ll hasil compiler, lalu di-link jadi satu exe.
 *
 * Model memori: tidak ada heap allocator untuk program. Runtime sendiri memakai malloc
 * hanya untuk catatan internal (pesan queue, catatan thread, daftar region per thread). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

_Noreturn void tl_abort(const char* msg, int64_t n);
void tl_panic_set(const char* msg, int64_t n);
/* 1 bila program memakai defer/recover: kegagalan alokasi menjadi panic (bisa di-recover) */
extern const int32_t tl_panic_mode;
#define TL_PANIC (-1)

/* ---------------- region data untuk array tumbuh [..]T ----------------
 * Setiap array tumbuh memiliki satu region alamat virtual yang dicadangkan sekali
 * (default 4 GiB) dan di-commit bertahap. Region tidak pernah dipindah, jadi data tidak
 * pernah berpindah saat array tumbuh. Membebaskan = mengembalikan region ke pool.
 * Pool dan daftar region bersifat per thread (tidak ada memori bersama antar thread). */
typedef struct {
    char* data;
    int64_t len;
    int64_t cap; /* byte yang sudah di-commit */
} tl_dyn;

#define TL_POOL_MAX 64
#define TL_KEEP_COMMITTED (64 * 1024)
static int64_t tl_reserve = (int64_t)4 << 30;
static _Thread_local struct {
    char* base;
    int64_t committed;
} tl_pool[TL_POOL_MAX];
static _Thread_local int tl_pool_n = 0;

/* region yang dimiliki goroutine yang sedang berjalan (dibebaskan bila goroutine itu abort) */
typedef struct {
    char** items;
    int64_t n, cap;
} tl_live_list;
static tl_live_list tl_boot_live;                 /* sebelum scheduler berjalan */
static _Thread_local tl_live_list* tl_cur_live = &tl_boot_live;

static void live_add(char* base) {
    tl_live_list* l = tl_cur_live;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (char**)realloc(l->items, (size_t)l->cap * sizeof(char*));
        if (!l->items) {
            fputs("out of memory\n", stderr);
            exit(101);
        }
    }
    l->items[l->n++] = base;
}

static void live_remove(char* base) {
    tl_live_list* l = tl_cur_live;
    for (int64_t i = l->n; i-- > 0;)
        if (l->items[i] == base) {
            l->items[i] = l->items[--l->n];
            return;
        }
}

static char* region_reserve(void) {
#ifdef _WIN32
    return (char*)VirtualAlloc(NULL, (SIZE_T)tl_reserve, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(NULL, (size_t)tl_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : (char*)p;
#endif
}

static int region_commit(char* base, int64_t from, int64_t to) {
#ifdef _WIN32
    return VirtualAlloc(base + from, (SIZE_T)(to - from), MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return mprotect(base + from, (size_t)(to - from), PROT_READ | PROT_WRITE) == 0;
#endif
}

static void region_decommit(char* base, int64_t from, int64_t to) {
#ifdef _WIN32
    VirtualFree(base + from, (SIZE_T)(to - from), MEM_DECOMMIT);
#else
    madvise(base + from, (size_t)(to - from), MADV_DONTNEED);
    mprotect(base + from, (size_t)(to - from), PROT_NONE);
#endif
}

static void region_release(char* base) {
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, (size_t)tl_reserve);
#endif
}

/* Pastikan minimal 'need' byte ter-commit. Data lama tidak berpindah. */
/* Kegagalan alokasi: panic (mode panic, array tidak berubah) atau abort. */
static int32_t alloc_fail(const char* msg, int64_t n) {
    if (tl_panic_mode) {
        tl_panic_set(msg, n);
        return 0;
    }
    tl_abort(msg, n);
}

/* Pastikan minimal 'need' byte ter-commit. Data lama tidak berpindah. 1 = berhasil. */
int32_t tl_dyn_reserve(tl_dyn* d, int64_t need) {
    static const char too_big[] = "growable array exceeds its reserved address space (TL_RESERVE_MB)";
    static const char oom[] = "out of memory";
    if (need <= d->cap) return 1;
    if (need < 0 || need > tl_reserve) return alloc_fail(too_big, (int64_t)sizeof too_big - 1);
    if (!d->data) {
        if (tl_pool_n > 0) {
            tl_pool_n--;
            d->data = tl_pool[tl_pool_n].base;
            d->cap = tl_pool[tl_pool_n].committed;
            live_add(d->data);
            if (need <= d->cap) return 1;
        } else {
            char* base = region_reserve();
            if (!base) return alloc_fail(oom, (int64_t)sizeof oom - 1);
            d->data = base;
            d->cap = 0;
            live_add(d->data);
        }
    }
    int64_t cap = d->cap < 4096 ? 4096 : d->cap * 2;
    if (cap < need) cap = need;
    cap = (cap + 4095) & ~(int64_t)4095;
    if (cap > tl_reserve) cap = tl_reserve;
    if (!region_commit(d->data, d->cap, cap)) return alloc_fail(oom, (int64_t)sizeof oom - 1);
    d->cap = cap;
    return 1;
}

void tl_dyn_free(tl_dyn* d) {
    if (!d->data) return;
    live_remove(d->data);
    int64_t keep = d->cap < TL_KEEP_COMMITTED ? d->cap : TL_KEEP_COMMITTED;
    if (tl_pool_n < TL_POOL_MAX) {
        if (d->cap > keep) region_decommit(d->data, keep, d->cap);
        tl_pool[tl_pool_n].base = d->data;
        tl_pool[tl_pool_n].committed = keep;
        tl_pool_n++;
    } else {
        region_release(d->data);
    }
    d->data = NULL;
    d->len = 0;
    d->cap = 0;
}

/* Lepaskan semua region yang masih dimiliki sebuah goroutine (selesai atau abort). */
static void release_live(tl_live_list* l) {
    for (int64_t i = 0; i < l->n; i++) region_release(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

/* Kepemilikan region berpindah antar goroutine lewat channel (move [..]T). */
void tl_region_disown(char* base) {
    if (base) live_remove(base);
}
void tl_region_own(char* base) {
    if (base) live_add(base);
}

/* ---------------- to_bytes / from_bytes ----------------
 * Serializer/deserializer per tipe dihasilkan compiler; runtime hanya menyalin byte. */
typedef struct {
    const char* data;
    int64_t len;
    int64_t pos;
    int64_t link_limit; /* jumlah elemen: setiap Link[T] harus < nilai ini */
} tl_cursor;

int32_t tl_bytes_put(tl_dyn* out, const void* p, int64_t n) {
    if (!tl_dyn_reserve(out, out->len + n)) return 0; /* panic: tidak menulis apa pun */
    memcpy(out->data + out->len, p, (size_t)n);
    out->len += n;
    return 1;
}

/* 1 = berhasil; 0 = data habis (dst tidak diubah) */
int32_t tl_bytes_take(tl_cursor* c, void* dst, int64_t n) {
    if (n < 0 || c->pos > c->len - n) return 0;
    memcpy(dst, c->data + c->pos, (size_t)n);
    c->pos += n;
    return 1;
}

/* ---------------- sinkronisasi ---------------- */
#ifdef _WIN32
typedef SRWLOCK tl_lock;
typedef CONDITION_VARIABLE tl_cond;
static void lock_init(tl_lock* l) { InitializeSRWLock(l); }
static void lock_on(tl_lock* l) { AcquireSRWLockExclusive(l); }
static void lock_off(tl_lock* l) { ReleaseSRWLockExclusive(l); }
static void cond_init(tl_cond* c) { InitializeConditionVariable(c); }
static void cond_wait(tl_cond* c, tl_lock* l) { SleepConditionVariableSRW(c, l, INFINITE, 0); }
static void cond_wait_ms(tl_cond* c, tl_lock* l, int64_t ms) {
    SleepConditionVariableSRW(c, l, ms < 0 ? INFINITE : (DWORD)(ms > 0x7fffffff ? 0x7fffffff : ms), 0);
}
static void cond_signal(tl_cond* c) { WakeConditionVariable(c); }
static void cond_broadcast(tl_cond* c) { WakeAllConditionVariable(c); }
#else
typedef pthread_mutex_t tl_lock;
typedef pthread_cond_t tl_cond;
static void lock_init(tl_lock* l) { pthread_mutex_init(l, NULL); }
static void lock_on(tl_lock* l) { pthread_mutex_lock(l); }
static void lock_off(tl_lock* l) { pthread_mutex_unlock(l); }
static void cond_init(tl_cond* c) { pthread_cond_init(c, NULL); }
static void cond_wait(tl_cond* c, tl_lock* l) { pthread_cond_wait(c, l); }
static void cond_wait_ms(tl_cond* c, tl_lock* l, int64_t ms) {
    if (ms < 0) {
        pthread_cond_wait(c, l);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(c, l, &ts);
}
static void cond_signal(tl_cond* c) { pthread_cond_signal(c); }
static void cond_broadcast(tl_cond* c) { pthread_cond_broadcast(c); }
#endif

/* satu print = satu baris utuh, tidak terpotong oleh thread lain */
static tl_lock tl_print_mutex;
void tl_print_lock(void) { lock_on(&tl_print_mutex); }
void tl_print_unlock(void) { lock_off(&tl_print_mutex); }

/* ---------------- goroutine: scheduler M:N ----------------
 * Goroutine = fiber dengan stack sendiri (ukuran dari analisis stack compiler), dijadwalkan
 * di atas N worker OS thread lewat satu run-queue. Goroutine hanya berpindah pada titik
 * tunggu (channel, select, wait, sleep), jadi tidak ada memori bersama yang bisa berpacu:
 * satu-satunya yang dibagi adalah objek channel/task milik runtime, dan semuanya dikunci. */
#ifdef _WIN32
typedef LPVOID tl_fiber;
#else
#include <ucontext.h>
typedef struct {
    ucontext_t ctx;
    void* stack;
    size_t size;
} * tl_fiber;
#endif

typedef struct tl_task tl_task;
typedef struct tl_g {
    tl_fiber fiber;
    void (*entry)(void*);
    void* args;
    tl_task* task;
    jmp_buf* jb;
    tl_live_list live;
    int64_t* depth;
    int32_t depth_n;
    int is_main;
    int finished;
    volatile long running;      /* 1 selama stack goroutine ini dipakai sebuah worker */
    int32_t unwind;             /* 'return ... from' yang sedang berjalan (id target), TL_PANIC = panic */
    char* panic_msg;            /* pesan panic yang sedang berjalan (malloc, milik runtime) */
    int64_t panic_len;
    char* from_vals;            /* slot nilai 'return ... from' per target */
    /* parkir: siapa pun yang membangunkan harus 'mengklaim' dulu (select/timer bisa berebut) */
    volatile long claimed;
    int32_t wake_case;
    int32_t wake_ok;
    uint64_t park_seq;
    struct tl_g* next;
} tl_g;

static struct {
    tl_lock lock;
    tl_cond work;
    tl_g *head, *tail;
    int workers, idle, live;
    int ext; /* sumber kejadian di luar goroutine (mis. thread I/O http): selama > 0 bukan deadlock */
    struct {
        tl_g* g;
        uint64_t seq;
        int64_t at;
    } * timers;
    int ntimers, captimers;
} tl_s;

static _Thread_local tl_g* tl_cur_g = NULL;
static _Thread_local tl_fiber tl_sched_fiber = NULL;
static _Thread_local tl_lock* tl_pending_unlock = NULL;
static int64_t tl_main_stack_bytes = 1 << 20;

int64_t tl_os_clock_ns(void);
static int64_t now_ms(void) { return tl_os_clock_ns() / 1000000; }

#ifdef _WIN32
static void fiber_switch(tl_fiber to) { SwitchToFiber(to); }
static void fiber_delete(tl_fiber f) { DeleteFiber(f); }
static tl_fiber fiber_self(void) { return ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH); }
static void WINAPI g_main(void* p);
static tl_fiber fiber_new(size_t stack, tl_g* g) { return CreateFiberEx(64 * 1024, stack, FIBER_FLAG_FLOAT_SWITCH, g_main, g); }
#else
static _Thread_local tl_fiber tl_cur_fiber = NULL;
static void g_main(void* p);
static void fiber_tramp(unsigned hi, unsigned lo) { g_main((void*)(((uintptr_t)hi << 32) | lo)); }
static void fiber_switch(tl_fiber to) {
    tl_fiber from = tl_cur_fiber;
    tl_cur_fiber = to;
    swapcontext(&from->ctx, &to->ctx);
}
static void fiber_delete(tl_fiber f) {
    munmap(f->stack, f->size);
    free(f);
}
static tl_fiber fiber_self(void) {
    tl_fiber f = calloc(1, sizeof *f);
    getcontext(&f->ctx);
    tl_cur_fiber = f;
    return f;
}
static tl_fiber fiber_new(size_t stack, tl_g* g) {
    tl_fiber f = calloc(1, sizeof *f);
    f->size = stack;
    f->stack = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = stack;
    uintptr_t v = (uintptr_t)g;
    makecontext(&f->ctx, (void (*)(void))fiber_tramp, 2, (unsigned)(v >> 32), (unsigned)v);
    return f;
}
#endif

static void runq_push_locked(tl_g* g) {
    g->next = NULL;
    if (tl_s.tail) tl_s.tail->next = g;
    else tl_s.head = g;
    tl_s.tail = g;
    cond_signal(&tl_s.work);
}

/* Bangunkan g bila belum diklaim pihak lain. case_id/ok = hasil untuk g. */
static int g_wake(tl_g* g, int32_t case_id, int32_t ok) {
#ifdef _WIN32
    if (InterlockedCompareExchange(&g->claimed, 1, 0) != 0) return 0;
#else
    if (!__sync_bool_compare_and_swap(&g->claimed, 0, 1)) return 0;
#endif
    g->wake_case = case_id;
    g->wake_ok = ok;
    lock_on(&tl_s.lock);
    runq_push_locked(g);
    lock_off(&tl_s.lock);
    return 1;
}

/* Parkir goroutine ini; 'held' dilepas oleh scheduler SETELAH stack ini tidak lagi berjalan. */
static void g_park(tl_lock* held) {
    tl_pending_unlock = held;
    fiber_switch(tl_sched_fiber);
    /* dilanjutkan oleh worker mana pun */
}

static void g_prepare_park(void) {
    tl_cur_g->claimed = 0;
    tl_cur_g->park_seq++;
}

static void timer_add(tl_g* g, int64_t at) {
    lock_on(&tl_s.lock);
    if (tl_s.ntimers == tl_s.captimers) {
        tl_s.captimers = tl_s.captimers ? tl_s.captimers * 2 : 16;
        tl_s.timers = realloc(tl_s.timers, (size_t)tl_s.captimers * sizeof *tl_s.timers);
    }
    tl_s.timers[tl_s.ntimers].g = g;
    tl_s.timers[tl_s.ntimers].seq = g->park_seq;
    tl_s.timers[tl_s.ntimers].at = at;
    tl_s.ntimers++;
    lock_off(&tl_s.lock);
}

/* (sched terkunci) jalankan timer yang sudah lewat; kembalikan ms ke timer berikutnya (-1 = tidak ada) */
static int64_t timers_fire_locked(void) {
    int64_t t = now_ms(), next = -1;
    for (int i = 0; i < tl_s.ntimers;) {
        if (tl_s.timers[i].at <= t) {
            tl_g* g = tl_s.timers[i].g;
            uint64_t seq = tl_s.timers[i].seq;
            tl_s.timers[i] = tl_s.timers[--tl_s.ntimers];
#ifdef _WIN32
            int mine = g->park_seq == seq && InterlockedCompareExchange(&g->claimed, 1, 0) == 0;
#else
            int mine = g->park_seq == seq && __sync_bool_compare_and_swap(&g->claimed, 0, 1);
#endif
            if (mine) {
                g->wake_case = -2; /* timeout */
                g->wake_ok = 1;
                runq_push_locked(g);
            }
            continue;
        }
        int64_t d = tl_s.timers[i].at - t;
        if (next < 0 || d < next) next = d;
        i++;
    }
    return next;
}

static void worker_loop(void) {
    tl_sched_fiber = fiber_self();
    for (;;) {
        lock_on(&tl_s.lock);
        tl_g* g = NULL;
        for (;;) {
            int64_t wait = timers_fire_locked();
            if (tl_s.head) {
                g = tl_s.head;
                tl_s.head = g->next;
                if (!tl_s.head) tl_s.tail = NULL;
                break;
            }
            tl_s.idle++;
            if (tl_s.idle == tl_s.workers && tl_s.ntimers == 0 && tl_s.live > 0 && tl_s.ext == 0) {
                fflush(stdout);
                fprintf(stderr, "abort: deadlock: all %d goroutine(s) are blocked forever "
                                "(waiting on channels or tasks that nothing will ever complete)\n", tl_s.live);
                exit(101);
            }
            cond_wait_ms(&tl_s.work, &tl_s.lock, wait);
            tl_s.idle--;
        }
        lock_off(&tl_s.lock);
        /* g bisa sudah masuk run-queue sebelum selesai parkir di worker lain: tunggu stack-nya bebas */
#ifdef _WIN32
        while (InterlockedCompareExchange(&g->running, 1, 0) != 0) SwitchToThread();
#else
        while (!__sync_bool_compare_and_swap(&g->running, 0, 1)) sched_yield();
#endif
        tl_cur_g = g;
        tl_cur_live = &g->live;
        fiber_switch(g->fiber);
        tl_cur_g = NULL;
        tl_cur_live = &tl_boot_live;
        if (tl_pending_unlock) {
            lock_off(tl_pending_unlock);
            tl_pending_unlock = NULL;
        }
        if (g->finished) {
            fiber_delete(g->fiber);
            free(g->depth);
            free(g->from_vals);
            free(g);
        } else {
#ifdef _WIN32
            InterlockedExchange(&g->running, 0);
#else
            __sync_lock_release(&g->running);
#endif
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_thread(void* p) {
    (void)p;
    worker_loop();
    return 0;
}
#else
static void* worker_thread(void* p) {
    (void)p;
    worker_loop();
    return NULL;
}
#endif

/* counter #max_depth per goroutine (fungsi rekursif bisa parkir di tengah rekursi) */
extern const int64_t tl_depth_slots; /* jumlah fungsi #max_depth (dari compiler) */
extern const int64_t tl_from_bytes;  /* ukuran total slot 'return ... from' (dari compiler) */
static int64_t tl_boot_depth[256];
static int32_t tl_boot_unwind;
static char tl_boot_from[4096];

/* Array berukuran tetap sejak goroutine dibuat: pointer yang di-cache per frame tetap valid. */
int64_t* tl_depth_ptr(int32_t id) {
    tl_g* g = tl_cur_g;
    if (!g || id >= g->depth_n) return &tl_boot_depth[id & 255];
    return &g->depth[id];
}

/* state 'return ... from' per goroutine (bukan thread_local: goroutine berpindah worker) */
int32_t* tl_unwind_ptr(void) { return tl_cur_g ? &tl_cur_g->unwind : &tl_boot_unwind; }
char* tl_from_slot(int64_t offset) {
    tl_g* g = tl_cur_g;
    if (!g || !g->from_vals) return tl_boot_from + offset;
    return g->from_vals + offset;
}

/* ---------------- task (hasil 'run f(...)') ----------------
 * Handle = (generasi << 32) | slot. Slot tidak pernah dibebaskan dari memori (hanya dipakai ulang),
 * jadi handle basi aman dibaca dan memberi hasil 'sudah tidak berlaku'. */
struct tl_task {
    tl_lock lock;
    uint32_t gen;
    int in_use, done, ok, waited;
    tl_g* waiter;
};
static struct {
    tl_lock lock;
    tl_task** slots;
    uint32_t n, cap;
    uint32_t* free;
    uint32_t nfree, capfree;
} tl_tasks;

static int64_t task_new(tl_task** out) {
    lock_on(&tl_tasks.lock);
    uint32_t idx;
    if (tl_tasks.nfree) {
        idx = tl_tasks.free[--tl_tasks.nfree];
    } else {
        if (tl_tasks.n == tl_tasks.cap) {
            tl_tasks.cap = tl_tasks.cap ? tl_tasks.cap * 2 : 64;
            tl_tasks.slots = realloc(tl_tasks.slots, tl_tasks.cap * sizeof(tl_task*));
        }
        idx = tl_tasks.n++;
        tl_tasks.slots[idx] = calloc(1, sizeof(tl_task));
        lock_init(&tl_tasks.slots[idx]->lock);
    }
    tl_task* t = tl_tasks.slots[idx];
    lock_off(&tl_tasks.lock);
    lock_on(&t->lock);
    t->gen++;
    t->in_use = 1;
    t->done = t->ok = t->waited = 0;
    t->waiter = NULL;
    uint32_t gen = t->gen;
    lock_off(&t->lock);
    *out = t;
    return (int64_t)(((uint64_t)gen << 32) | idx);
}

static tl_task* task_get(int64_t h, uint32_t* gen) {
    uint32_t idx = (uint32_t)h;
    *gen = (uint32_t)((uint64_t)h >> 32);
    lock_on(&tl_tasks.lock);
    tl_task* t = idx < tl_tasks.n ? tl_tasks.slots[idx] : NULL;
    lock_off(&tl_tasks.lock);
    return t;
}

static void task_release_locked(tl_task* t, uint32_t idx) {
    t->in_use = 0;
    t->gen++;
    lock_on(&tl_tasks.lock);
    if (tl_tasks.nfree == tl_tasks.capfree) {
        tl_tasks.capfree = tl_tasks.capfree ? tl_tasks.capfree * 2 : 64;
        tl_tasks.free = realloc(tl_tasks.free, tl_tasks.capfree * sizeof(uint32_t));
    }
    tl_tasks.free[tl_tasks.nfree++] = idx;
    lock_off(&tl_tasks.lock);
}

/* wait(t): 1 = goroutine selesai normal; 0 = abort, sudah pernah di-wait, atau handle basi */
int32_t tl_wait(int64_t h) {
    uint32_t gen;
    tl_task* t = task_get(h, &gen);
    if (!t) return 0;
    lock_on(&t->lock);
    if (!t->in_use || t->gen != gen || t->waited) {
        lock_off(&t->lock);
        return 0;
    }
    t->waited = 1;
    tl_g* me = tl_cur_g;
    while (!t->done) {
        g_prepare_park();
        t->waiter = me;
        g_park(&t->lock);
        lock_on(&t->lock);
    }
    int ok = t->ok;
    task_release_locked(t, (uint32_t)h);
    lock_off(&t->lock);
    return ok;
}

#ifdef _WIN32
static void WINAPI g_main(void* p) {
#else
static void g_main(void* p) {
#endif
    tl_g* g = (tl_g*)p;
    jmp_buf jb;
    g->jb = &jb;
    int ok = 0;
    if (setjmp(jb) == 0) {
        g->entry(g->args);
        ok = 1;
        if (g->unwind == TL_PANIC) {
            /* panic yang tidak di-recover: semua frame sudah menjalankan defer & membebaskan miliknya */
            fflush(stdout);
            fputs(g->is_main ? "panic: " : "panic (goroutine): ", stderr);
            fwrite(g->panic_msg, 1, (size_t)g->panic_len, stderr);
            fputc('\n', stderr);
            if (g->is_main) exit(101);
            ok = 0;
        }
    }
    free(g->panic_msg);
    g->panic_msg = NULL;
    if (g->is_main) {
        fflush(stdout);
        exit(0); /* main selesai: program selesai (goroutine lain ikut berhenti) */
    }
    release_live(&g->live);
    free(g->args);
    tl_task* t = g->task;
    lock_on(&t->lock);
    t->done = 1;
    t->ok = ok;
    tl_g* w = t->waiter;
    t->waiter = NULL;
    lock_off(&t->lock);
    if (w) g_wake(w, 0, 1);
    lock_on(&tl_s.lock);
    tl_s.live--;
    lock_off(&tl_s.lock);
    g->finished = 1;
    fiber_switch(tl_sched_fiber);
}

static tl_g* g_new(void (*entry)(void*), void* args, int64_t stack) {
    tl_g* g = (tl_g*)calloc(1, sizeof(tl_g));
    if (!g) {
        static const char oom[] = "out of memory (goroutine)";
        tl_abort(oom, (int64_t)sizeof oom - 1);
    }
    g->entry = entry;
    g->args = args;
    g->depth_n = (int32_t)tl_depth_slots;
    if (g->depth_n > 0) g->depth = (int64_t*)calloc((size_t)g->depth_n, sizeof(int64_t));
    if (tl_from_bytes > 0) g->from_vals = (char*)calloc(1, (size_t)tl_from_bytes);
    if (stack < 128 * 1024) stack = 128 * 1024;
    g->fiber = fiber_new((size_t)stack, g);
    if (!g->fiber) {
        static const char fail[] = "cannot create a goroutine stack";
        tl_abort(fail, (int64_t)sizeof fail - 1);
    }
    return g;
}

/* run f(args): argumen sudah disalin; stack_bytes = perkiraan compiler + margin */
int64_t tl_go(void (*entry)(void*), const void* args, int64_t size, int64_t stack_bytes) {
    void* copy = malloc(size > 0 ? (size_t)size : 1);
    if (!copy) {
        static const char oom[] = "out of memory (goroutine)";
        tl_abort(oom, (int64_t)sizeof oom - 1);
    }
    if (size > 0) memcpy(copy, args, (size_t)size);
    tl_task* t;
    int64_t h = task_new(&t);
    tl_g* g = g_new(entry, copy, stack_bytes);
    g->task = t;
    lock_on(&tl_s.lock);
    tl_s.live++;
    runq_push_locked(g);
    lock_off(&tl_s.lock);
    return h;
}

/* ---------------- channel ----------------
 * Ring buffer berkapasitas tetap + antrean goroutine yang menunggu. Handle seperti task:
 * channel yang ditutup dan kosong dipakai ulang; handle basi bersikap seperti channel tertutup. */
typedef struct tl_waiter {
    tl_g* g;
    void* data;
    int32_t case_id;
    uint64_t seq;
    struct tl_waiter* next;
} tl_waiter;

typedef struct {
    tl_lock lock;
    uint32_t gen;
    int in_use, closed;
    int64_t elem, cap, head, count;
    char* buf;
    tl_waiter *recvq, *sendq;
} tl_chan;

static struct {
    tl_lock lock;
    tl_chan** slots;
    uint32_t n, cap;
    uint32_t* free;
    uint32_t nfree, capfree;
} tl_chans;

int64_t tl_chan_new(int64_t elem, int64_t cap) {
    if (cap < 0) cap = 0;
    lock_on(&tl_chans.lock);
    uint32_t idx;
    if (tl_chans.nfree) {
        idx = tl_chans.free[--tl_chans.nfree];
    } else {
        if (tl_chans.n == tl_chans.cap) {
            tl_chans.cap = tl_chans.cap ? tl_chans.cap * 2 : 64;
            tl_chans.slots = realloc(tl_chans.slots, tl_chans.cap * sizeof(tl_chan*));
        }
        idx = tl_chans.n++;
        tl_chans.slots[idx] = calloc(1, sizeof(tl_chan));
        lock_init(&tl_chans.slots[idx]->lock);
    }
    tl_chan* c = tl_chans.slots[idx];
    lock_off(&tl_chans.lock);
    lock_on(&c->lock);
    c->gen++;
    c->in_use = 1;
    c->closed = 0;
    c->elem = elem;
    c->cap = cap;
    c->head = c->count = 0;
    free(c->buf);
    c->buf = cap > 0 ? (char*)malloc((size_t)(cap * elem)) : NULL;
    if (cap > 0 && !c->buf) {
        lock_off(&c->lock);
        static const char oom[] = "out of memory (channel buffer)";
        tl_abort(oom, (int64_t)sizeof oom - 1);
    }
    c->recvq = c->sendq = NULL;
    uint32_t gen = c->gen;
    lock_off(&c->lock);
    return (int64_t)(((uint64_t)gen << 32) | idx);
}

/* kunci channel; NULL bila handle basi (perlakukan sebagai tertutup) */
static tl_chan* chan_lock(int64_t h) {
    uint32_t idx = (uint32_t)h, gen = (uint32_t)((uint64_t)h >> 32);
    lock_on(&tl_chans.lock);
    tl_chan* c = idx < tl_chans.n ? tl_chans.slots[idx] : NULL;
    lock_off(&tl_chans.lock);
    if (!c) return NULL;
    lock_on(&c->lock);
    if (!c->in_use || c->gen != gen) {
        lock_off(&c->lock);
        return NULL;
    }
    return c;
}

/* (c terkunci) channel tertutup, kosong, tanpa penunggu: kembalikan slotnya */
static void chan_maybe_release(tl_chan* c, uint32_t idx) {
    if (!c->closed || c->count || c->recvq || c->sendq) return;
    c->in_use = 0;
    c->gen++;
    lock_on(&tl_chans.lock);
    if (tl_chans.nfree == tl_chans.capfree) {
        tl_chans.capfree = tl_chans.capfree ? tl_chans.capfree * 2 : 64;
        tl_chans.free = realloc(tl_chans.free, tl_chans.capfree * sizeof(uint32_t));
    }
    tl_chans.free[tl_chans.nfree++] = idx;
    lock_off(&tl_chans.lock);
}

static void q_push(tl_waiter** q, tl_waiter* w) {
    w->next = NULL;
    while (*q) q = &(*q)->next;
    *q = w;
}

static void q_remove(tl_waiter** q, tl_waiter* w) {
    for (; *q; q = &(*q)->next)
        if (*q == w) {
            *q = w->next;
            return;
        }
}

/* ambil penunggu pertama yang masih bisa diklaim (penunggu basi dari select dibuang) */
static tl_waiter* q_claim(tl_waiter** q, int32_t ok) {
    while (*q) {
        tl_waiter* w = *q;
        *q = w->next;
        if (w->g->park_seq != w->seq) continue;
#ifdef _WIN32
        if (InterlockedCompareExchange(&w->g->claimed, 1, 0) != 0) continue;
#else
        if (!__sync_bool_compare_and_swap(&w->g->claimed, 0, 1)) continue;
#endif
        w->g->wake_case = w->case_id;
        w->g->wake_ok = ok;
        return w;
    }
    return NULL;
}

static void wake_claimed(tl_g* g) {
    lock_on(&tl_s.lock);
    runq_push_locked(g);
    lock_off(&tl_s.lock);
}

/* (c terkunci) coba kirim tanpa menunggu: 1 terkirim, 0 penuh, -1 tertutup */
static int try_send(tl_chan* c, const void* data) {
    if (c->closed) return -1;
    tl_waiter* r = q_claim(&c->recvq, 1);
    if (r) {
        memcpy(r->data, data, (size_t)c->elem);
        wake_claimed(r->g);
        return 1;
    }
    if (c->count < c->cap) {
        memcpy(c->buf + ((c->head + c->count) % c->cap) * c->elem, data, (size_t)c->elem);
        c->count++;
        return 1;
    }
    return 0;
}

/* (c terkunci) coba terima tanpa menunggu: 1 diterima, 0 kosong, -1 tertutup & kosong */
static int try_recv(tl_chan* c, void* data) {
    if (c->count > 0) {
        memcpy(data, c->buf + c->head * c->elem, (size_t)c->elem);
        c->head = (c->head + 1) % c->cap;
        c->count--;
        tl_waiter* s = q_claim(&c->sendq, 1);
        if (s) {
            memcpy(c->buf + ((c->head + c->count) % c->cap) * c->elem, s->data, (size_t)c->elem);
            c->count++;
            wake_claimed(s->g);
        }
        return 1;
    }
    tl_waiter* s = q_claim(&c->sendq, 1);
    if (s) {
        memcpy(data, s->data, (size_t)c->elem);
        wake_claimed(s->g);
        return 1;
    }
    return c->closed ? -1 : 0;
}

/* ch <- v : 1 terkirim, 0 channel tertutup */
int32_t tl_chan_send(int64_t h, const void* data) {
    tl_chan* c = chan_lock(h);
    if (!c) return 0;
    int r = try_send(c, data);
    if (r != 0) {
        lock_off(&c->lock);
        return r > 0;
    }
    tl_g* me = tl_cur_g;
    tl_waiter w = {me, (void*)data, 0, 0, NULL};
    g_prepare_park();
    w.seq = me->park_seq;
    q_push(&c->sendq, &w);
    g_park(&c->lock);
    return me->wake_ok;
}

/* Kirim tanpa menunggu dari thread OS mana pun (bukan goroutine), mis. thread I/O http.
   1 terkirim, 0 penuh (coba lagi nanti), -1 tertutup. */
int32_t tl_chan_offer(int64_t h, const void* data) {
    tl_chan* c = chan_lock(h);
    if (!c) return -1;
    int r = try_send(c, data);
    lock_off(&c->lock);
    return r;
}

/* Thread di luar scheduler yang akan membangunkan goroutine (+1 / -1). */
void tl_ext_hold(int32_t delta) {
    lock_on(&tl_s.lock);
    tl_s.ext += delta;
    lock_off(&tl_s.lock);
}

/* <-ch : 1 diterima, 0 tertutup & kosong */
int32_t tl_chan_recv(int64_t h, void* data) {
    tl_chan* c = chan_lock(h);
    if (!c) return 0;
    int r = try_recv(c, data);
    if (r != 0) {
        chan_maybe_release(c, (uint32_t)h);
        lock_off(&c->lock);
        return r > 0;
    }
    tl_g* me = tl_cur_g;
    tl_waiter w = {me, data, 0, 0, NULL};
    g_prepare_park();
    w.seq = me->park_seq;
    q_push(&c->recvq, &w);
    g_park(&c->lock);
    if (me->wake_ok == 0) {
        /* bangun karena close: lepaskan slot channel bila sudah kosong */
        tl_chan* c2 = chan_lock(h);
        if (c2) {
            chan_maybe_release(c2, (uint32_t)h);
            lock_off(&c2->lock);
        }
    }
    return me->wake_ok;
}

/* close(ch): penunggu dibangunkan (recv → none, send → false); close kedua kali = no-op */
void tl_chan_close(int64_t h) {
    tl_chan* c = chan_lock(h);
    if (!c) return;
    if (!c->closed) {
        c->closed = 1;
        tl_waiter* w;
        while ((w = q_claim(&c->recvq, 0))) wake_claimed(w->g);
        while ((w = q_claim(&c->sendq, 0))) wake_claimed(w->g);
    }
    chan_maybe_release(c, (uint32_t)h);
    lock_off(&c->lock);
}

/* select: kind 0 = recv, 1 = send. Mengembalikan indeks case; -2 = timeout, -3 = default. */
typedef struct {
    int32_t kind;
    int64_t ch;
    void* data;
} tl_case;

int32_t tl_select(tl_case* cases, int32_t n, int64_t timeout_ms, int32_t has_default, int32_t* ok) {
    /* 1) yang sudah siap, mulai dari posisi acak (adil) */
    int32_t start = n > 0 ? (int32_t)((uint64_t)tl_os_clock_ns() % (uint64_t)n) : 0;
    for (int32_t k = 0; k < n; k++) {
        int32_t i = (start + k) % n;
        tl_chan* c = chan_lock(cases[i].ch);
        if (!c) {
            /* channel basi = tertutup: recv siap dengan none, send gagal */
            *ok = 0;
            return i;
        }
        int r = cases[i].kind == 0 ? try_recv(c, cases[i].data) : try_send(c, cases[i].data);
        lock_off(&c->lock);
        if (r != 0) {
            *ok = r > 0;
            return i;
        }
    }
    if (has_default) return -3;
    if (n == 0 && timeout_ms < 0) {
        static const char msg[] = "select with no cases blocks forever";
        tl_abort(msg, (int64_t)sizeof msg - 1);
    }
    /* 2) parkir di semua channel sekaligus; pembangun pertama yang mengklaim menang */
    tl_waiter* ws = (tl_waiter*)calloc((size_t)(n > 0 ? n : 1), sizeof(tl_waiter));
    tl_g* me = tl_cur_g;
    g_prepare_park();
    uint64_t seq = me->park_seq;
    int32_t ready = -1;
    for (int32_t i = 0; i < n && ready < 0; i++) {
        tl_chan* c = chan_lock(cases[i].ch);
        if (!c) {
            ready = i;
            *ok = 0;
            break;
        }
        int r = cases[i].kind == 0 ? try_recv(c, cases[i].data) : try_send(c, cases[i].data);
        if (r != 0) {
            lock_off(&c->lock);
            ready = i;
            *ok = r > 0;
            break;
        }
        ws[i].g = me;
        ws[i].data = cases[i].data;
        ws[i].case_id = i;
        ws[i].seq = seq;
        q_push(cases[i].kind == 0 ? &c->recvq : &c->sendq, &ws[i]);
        lock_off(&c->lock);
    }
    if (ready < 0) {
        if (timeout_ms >= 0) timer_add(me, now_ms() + timeout_ms);
        /* parkir tanpa lock: penunggu sudah terdaftar; klaim atomik mencegah bangun ganda,
           flag 'running' mencegah worker lain memakai stack ini sebelum selesai parkir */
        g_park(NULL);
        ready = me->wake_case;
        *ok = me->wake_ok;
    } else {
        /* tidak jadi parkir: pastikan tidak ada yang mengklaim lewat penunggu yang sudah terdaftar */
#ifdef _WIN32
        if (InterlockedCompareExchange(&me->claimed, 1, 0) != 0) {
#else
        if (!__sync_bool_compare_and_swap(&me->claimed, 0, 1)) {
#endif
            /* pembangun lain sudah mengklaim dan memasukkan g ke run-queue: tunggu agar tidak berjalan ganda */
            g_park(NULL);
            ready = me->wake_case;
            *ok = me->wake_ok;
        }
    }
    /* 3) cabut semua pendaftaran; naikkan seq agar penunggu yang tersisa tidak berlaku */
    me->park_seq++;
    for (int32_t i = 0; i < n; i++) {
        if (!ws[i].g) continue;
        tl_chan* c = chan_lock(cases[i].ch);
        if (!c) continue;
        q_remove(cases[i].kind == 0 ? &c->recvq : &c->sendq, &ws[i]);
        lock_off(&c->lock);
    }
    free(ws);
    return ready;
}

/* ---------------- output ---------------- */
void tl_user_main(void);
static void tl_main_entry(void* p) {
    (void)p;
    tl_user_main();
}
/* ukuran stack goroutine main (diisi compiler lewat tl_set_main_stack di awal tl_user_main tidak
   mungkin: stack sudah dibuat), jadi compiler mengekspor simbol 'tl_main_stack' */
extern const int64_t tl_main_stack;

static char tl_stdout_buf[1 << 16];

/* str(...) mengalihkan output print ke sebuah [..]u8 (per thread) */
static _Thread_local tl_dyn* tl_cap = NULL;
void tl_capture_begin(tl_dyn* d) { tl_cap = d; }
void tl_capture_end(void) { tl_cap = NULL; }

static void tl_out(const char* s, size_t n) {
    if (tl_cap) tl_bytes_put(tl_cap, s, (int64_t)n);
    else fwrite(s, 1, n, stdout);
}

void tl_print_i64(int64_t v) {
    char b[32];
    int n = snprintf(b, sizeof b, "%lld", (long long)v);
    tl_out(b, (size_t)n);
}

void tl_print_u64(uint64_t v) {
    char b[32];
    int n = snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    tl_out(b, (size_t)n);
}

/* Representasi terpendek yang dibaca balik ke nilai yang sama persis. */
void tl_print_f64(double v) {
    char buf[48];
    if (v != v) {
        tl_out("nan", 3);
        return;
    }
    if (v == 1.0 / 0.0) {
        tl_out("inf", 3);
        return;
    }
    if (v == -1.0 / 0.0) {
        tl_out("-inf", 4);
        return;
    }
    /* jumlah digit signifikan terkecil yang round-trip */
    int prec = 1;
    for (; prec < 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (strtod(buf, NULL) == v) break;
    }
    snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
    int exp10 = atoi(strchr(buf, 'e') + 1);
    if (exp10 >= -5 && exp10 < 17) {
        /* notasi biasa: 10.0, 0.001, 123456789.0 */
        int decimals = prec - 1 - exp10;
        int n = snprintf(buf, sizeof buf, "%.*f", decimals > 0 ? decimals : 0, v);
        tl_out(buf, (size_t)n);
        if (!strchr(buf, '.')) tl_out(".0", 2);
    } else {
        /* notasi ilmiah: 1e+100, 1e-07 */
        int n = snprintf(buf, sizeof buf, "%.*g", prec, v);
        tl_out(buf, (size_t)n);
    }
}

void tl_print_bool(int32_t b) { tl_out(b ? "true" : "false", b ? 4 : 5); }

void tl_print_str(const char* s, int64_t n) { tl_out(s, (size_t)n); }

void tl_print_nl(void) { tl_out("\n", 1); }

/* ---------------- stdlib OS (dipanggil lewat pembungkus aman di prelude) ---------------- */
static int tl_argc = 0;
static char** tl_argv = NULL;

/* salin (ptr, len) menjadi string C ber-NUL; NULL bila ada byte 0 di tengah */
static char* cstr(const char* p, int64_t n) {
    if (n < 0 || (n > 0 && memchr(p, 0, (size_t)n))) return NULL;
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n > 0) memcpy(s, p, (size_t)n);
    s[n] = 0;
    return s;
}

static int32_t load_file(FILE* f, tl_dyn* out) {
    char buf[1 << 15];
    size_t n;
    out->len = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (!tl_bytes_put(out, buf, (int64_t)n)) return 0;
    return !ferror(f);
}

int32_t tl_os_read_file(const char* p, int64_t n, tl_dyn* out) {
    char* path = cstr(p, n);
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    free(path);
    if (!f) return 0;
    int32_t ok = load_file(f, out);
    fclose(f);
    return ok;
}

static int32_t store_file(const char* p, int64_t n, const char* d, int64_t dn, const char* mode) {
    char* path = cstr(p, n);
    if (!path) return 0;
    FILE* f = fopen(path, mode);
    free(path);
    if (!f) return 0;
    int32_t ok = dn == 0 || fwrite(d, 1, (size_t)dn, f) == (size_t)dn;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

int32_t tl_os_write_file(const char* p, int64_t n, const char* d, int64_t dn) { return store_file(p, n, d, dn, "wb"); }
int32_t tl_os_append_file(const char* p, int64_t n, const char* d, int64_t dn) { return store_file(p, n, d, dn, "ab"); }

int32_t tl_os_file_exists(const char* p, int64_t n) {
    char* path = cstr(p, n);
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    free(path);
    if (f) fclose(f);
    return f != NULL;
}

int32_t tl_os_delete_file(const char* p, int64_t n) {
    char* path = cstr(p, n);
    if (!path) return 0;
    int r = remove(path);
    free(path);
    return r == 0;
}

/* satu baris dari stdin tanpa newline; 0 bila EOF tanpa data */
int32_t tl_os_read_line(tl_dyn* out) {
    out->len = 0;
    int c, any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        any = 1;
        if (c == '\n') break;
        char ch = (char)c;
        if (!tl_bytes_put(out, &ch, 1)) return 0;
    }
    if (out->len > 0 && out->data[out->len - 1] == '\r') out->len--;
    return any;
}

int64_t tl_os_arg_count(void) { return tl_argc; }

int32_t tl_os_arg(int64_t i, tl_dyn* out) {
    if (i < 0 || i >= tl_argc) return 0;
    out->len = 0;
    tl_bytes_put(out, tl_argv[i], (int64_t)strlen(tl_argv[i]));
    return 1;
}

int32_t tl_os_env(const char* p, int64_t n, tl_dyn* out) {
    char* name = cstr(p, n);
    if (!name) return 0;
    const char* v = getenv(name);
    free(name);
    if (!v) return 0;
    out->len = 0;
    tl_bytes_put(out, v, (int64_t)strlen(v));
    return 1;
}

int64_t tl_os_time_ns(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100ns sejak 1601 */
    return (int64_t)((t - 116444736000000000ULL) * 100);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

int64_t tl_os_clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

void tl_os_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    if (tl_cur_g) {
        tl_g* me = tl_cur_g;
        g_prepare_park();
        timer_add(me, now_ms() + ms);
        g_park(NULL);
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)(ms > 0x7fffffff ? 0x7fffffff : ms));
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

uint64_t tl_os_random_seed(void) {
    uint64_t x = (uint64_t)tl_os_time_ns() ^ ((uint64_t)tl_os_clock_ns() << 17) ^ (uint64_t)(uintptr_t)&x;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

void tl_os_write_stderr(const char* p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
}

/* angka desimal ketat: tanda opsional + digit saja; overflow = gagal */
int32_t tl_parse_int(const char* p, int64_t n, int64_t* out) {
    int64_t i = 0;
    int neg = 0;
    if (n <= 0) return 0;
    if (p[0] == '-' || p[0] == '+') {
        neg = p[0] == '-';
        i = 1;
        if (n == 1) return 0;
    }
    uint64_t v = 0, lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        uint64_t d = (uint64_t)(p[i] - '0');
        if (v > (lim - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 1;
}

int32_t tl_parse_float(const char* p, int64_t n, double* out) {
    char* s = cstr(p, n);
    if (!s || n == 0) {
        free(s);
        return 0;
    }
    char* end;
    double v = strtod(s, &end);
    int ok = *end == 0 && end != s;
    free(s);
    if (ok) *out = v;
    return ok;
}

/* ---------------- penghentian ---------------- */

/* abort(...) eksplisit: di thread utama menghentikan program; di thread pekerja hanya thread itu. */
_Noreturn void tl_abort(const char* msg, int64_t n) {
    fflush(stdout);
    int worker = tl_cur_g && !tl_cur_g->is_main;
    fputs(worker ? "abort (goroutine): " : "abort: ", stderr);
    fwrite(msg, 1, (size_t)n, stderr);
    fputc('\n', stderr);
    if (worker) longjmp(*tl_cur_g->jb, 1);
    exit(101);
}

/* ---------------- panic ---------------- */

static char* tl_boot_panic_msg;
static int64_t tl_boot_panic_len;

/* Mulai (atau ganti) panic pada goroutine ini. Frame-frame membuka diri lewat flag unwind. */
void tl_panic_set(const char* msg, int64_t n) {
    tl_g* g = tl_cur_g;
    char** pm = g ? &g->panic_msg : &tl_boot_panic_msg;
    int64_t* pl = g ? &g->panic_len : &tl_boot_panic_len;
    char* copy = (char*)malloc(n > 0 ? (size_t)n : 1);
    if (copy && n > 0) memcpy(copy, msg, (size_t)n);
    free(*pm);
    *pm = copy;
    *pl = copy ? n : 0;
    *tl_unwind_ptr() = TL_PANIC;
}

/* recover(): pindahkan pesan panic ke [..]u8 baru. Compiler sudah memastikan sedang panic. */
void tl_recover(tl_dyn* out) {
    tl_g* g = tl_cur_g;
    char** pm = g ? &g->panic_msg : &tl_boot_panic_msg;
    int64_t* pl = g ? &g->panic_len : &tl_boot_panic_len;
    char* m = *pm;
    int64_t l = *pl;
    *pm = NULL;
    *pl = 0;
    out->len = 0;
    /* tidak muat (kehabisan memori) → panic baru "out of memory", ditangani compiler seperti
       panic lain di dalam defer */
    if (l > 0 && tl_dyn_reserve(out, l)) {
        memcpy(out->data, m, (size_t)l);
        out->len = l;
    }
    free(m);
}

/* panic di program tanpa defer/recover: tidak ada yang bisa menangkap, jadi langsung fatal. */
_Noreturn void tl_panic_fatal(const char* msg, int64_t n) {
    fflush(stdout);
    int worker = tl_cur_g && !tl_cur_g->is_main;
    fputs(worker ? "panic (goroutine): " : "panic: ", stderr);
    fwrite(msg, 1, (size_t)n, stderr);
    fputc('\n', stderr);
    if (worker) longjmp(*tl_cur_g->jb, 1);
    exit(101);
}

/* Hanya terjadi dengan --debug bila sebuah bukti compiler ternyata salah (bug compiler). */
_Noreturn void tl_internal_fail(int32_t code) {
    static const char* what[] = {"?", "integer overflow", "division by zero", "shift out of range",
                                 "narrowing cast out of range", "code proven unreachable was reached",
                                 "array index out of bounds"};
    fflush(stdout);
    fprintf(stderr,
            "internal compiler error: a property proven at compile time failed at runtime (%s).\n"
            "This is a bug in forest, please report it.\n",
            (code >= 1 && code <= 6) ? what[code] : what[0]);
    exit(102);
}

int main(int argc, char** argv) {
    tl_argc = argc;
    tl_argv = argv;
    setvbuf(stdout, tl_stdout_buf, _IOFBF, sizeof tl_stdout_buf);
    const char* mb = getenv("TL_RESERVE_MB");
    if (mb && atoll(mb) > 0) tl_reserve = (int64_t)atoll(mb) << 20;
    lock_init(&tl_print_mutex);
    lock_init(&tl_s.lock);
    cond_init(&tl_s.work);
    lock_init(&tl_tasks.lock);
    lock_init(&tl_chans.lock);
    /* main juga goroutine (agar bisa menunggu channel); worker = jumlah core */
    int n = 4;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = (int)si.dwNumberOfProcessors;
#else
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    if (c > 0) n = (int)c;
#endif
    if (n < 2) n = 2;
    if (n > 64) n = 64;
    const char* w = getenv("TL_WORKERS");
    if (w && atoi(w) > 0) n = atoi(w);
    tl_s.workers = n;
    tl_main_stack_bytes = tl_main_stack;
    tl_g* g = g_new(tl_main_entry, NULL, tl_main_stack_bytes);
    g->is_main = 1;
    tl_s.live = 1;
    runq_push_locked(g);
    for (int i = 1; i < n; i++) {
#ifdef _WIN32
        CloseHandle(CreateThread(NULL, 0, worker_thread, NULL, 0, NULL));
#else
        pthread_t t;
        pthread_create(&t, NULL, worker_thread, NULL);
#endif
    }
    worker_loop();
    return 0;
}
