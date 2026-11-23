/*
 * bench.c — microbenchmarks do artigo "Custo da virtualização em Apple Silicon"
 * Uso: ./bench [cache|branch|mem|flops|matmul|scale|all]
 * Saída: CSV "teste,parametro,valor" em stdout.
 * Compilar: clang -O2 -pthread -o bench bench.c -lm
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static uint64_t rng = 88172645463325252ULL;
static uint64_t xorshift(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, int n) {
    qsort(v, n, sizeof(double), cmp_dbl);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* ---------- Eixo 3: cache (pointer chasing, latência por acesso) ---------- */
typedef struct node { struct node *next; char pad[120]; } node; /* 128 B = linha do M-series */

static void bench_cache(void) {
    static const size_t kb[] = {4, 8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
                                1024, 2048, 4096, 6144, 8192, 12288, 16384, 24576, 32768,
                                49152, 65536, 131072, 262144};
    const long steps = 10000000;
    for (size_t s = 0; s < sizeof kb / sizeof kb[0]; s++) {
        size_t n = kb[s] * 1024 / sizeof(node);
        node *a = malloc(n * sizeof(node));
        size_t *perm = malloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++) perm[i] = i;
        for (size_t i = n - 1; i > 0; i--) { /* Sattolo: ciclo único aleatório */
            size_t j = xorshift() % i;
            size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (size_t i = 0; i < n; i++) a[i].next = &a[perm[i]];
        node *p = &a[0];
        for (long i = 0; i < steps / 10; i++) p = p->next; /* aquecimento */
        double t0 = now();
        for (long i = 0; i < steps; i++) p = p->next;
        double t1 = now();
        printf("cache,%zu,%.3f\n", kb[s], (t1 - t0) * 1e9 / steps + (p == NULL));
        free(a); free(perm);
    }
}

/* ---------- Eixo 2: CPU/ILP (array ordenado x aleatório) ---------- */
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static double branch_run(const int *d, int n, int reps) {
    long long sum = 0;
    double t0 = now();
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < n; i++)
            if (d[i] >= 128) { sum += d[i]; __asm__ volatile("" ::: "memory"); } /* força desvio real */
    double t1 = now();
    if (sum == 42) puts("");
    return (t1 - t0) * 1e9 / ((double)n * reps);
}

static void bench_branch(void) {
    const int n = 1 << 16, reps = 2000;
    int *d = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) d[i] = xorshift() % 256;
    printf("branch,unsorted,%.4f\n", branch_run(d, n, reps));
    qsort(d, n, sizeof(int), cmp_int);
    printf("branch,sorted,%.4f\n", branch_run(d, n, reps));
    free(d);
}

/* ---------- Eixo 4: memória / memória virtual ---------- */
static void bench_mem(void) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t bytes = (size_t)512 << 20;
    size_t pages = bytes / pg;
    printf("mem,pagesize,%ld\n", pg);

    char *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    double t0 = now();
    for (size_t i = 0; i < pages; i++) m[i * pg] = 1; /* 1º toque: page fault + zerar página */
    double t1 = now();
    for (size_t i = 0; i < pages; i++) m[i * pg] = 2; /* 2º toque: página já mapeada */
    double t2 = now();
    printf("mem,fault_ns_per_page,%.2f\n", (t1 - t0) * 1e9 / pages);
    printf("mem,touch_ns_per_page,%.2f\n", (t2 - t1) * 1e9 / pages);
    printf("mem,fault_ns_per_kib,%.2f\n", (t1 - t0) * 1e9 / (bytes / 1024.0));
    munmap(m, bytes);

    /* STREAM triad: a = b + s*c  (24 B e 2 FLOP por elemento) */
    size_t n = 16u << 20; /* 16 Mi doubles = 128 MiB por vetor */
    double *a = malloc(n * 8), *b = malloc(n * 8), *c = malloc(n * 8);
    for (size_t i = 0; i < n; i++) { a[i] = 0; b[i] = 1; c[i] = 2; }
    double v[5];
    for (int r = 0; r < 5; r++) {
        double s = 3.0 + r;
        double t3 = now();
        for (size_t i = 0; i < n; i++) a[i] = b[i] + s * c[i];
        double t4 = now();
        v[r] = 24.0 * n / (t4 - t3) / 1e9;
    }
    printf("mem,triad_gbs,%.3f\n", median(v, 5) + (a[n / 2] == -1));
    free(a); free(b); free(c);
}

/* ---------- Teto de computação (FMA vetorial independentes) ----------
 * 16 vetores de 2 doubles = 32 cadeias independentes de FMA (64 FLOP por rep).
 * m e k vêm de variáveis volatile: sem isso o clang calcula tudo em tempo
 * de compilação e o laço some (medido 0 s no arm64). */
#define CH 32
#define NV (CH / 2)
typedef double v2d __attribute__((vector_size(16)));
volatile double sink;
volatile double vmul = 0.999999999, vadd = 1e-9;
__attribute__((noinline)) static double fma_kernel(long reps) {
    const double m0 = vmul, k0 = vadd;
    const v2d m = {m0, m0}, k = {k0, k0};
    v2d x[NV];
    for (int i = 0; i < NV; i++) { x[i][0] = 1.0 + i * k0; x[i][1] = 1.0 - i * k0; }
    for (long r = 0; r < reps; r++)
        for (int i = 0; i < NV; i++) x[i] = x[i] * m + k;
    double s = 0;
    for (int i = 0; i < NV; i++) s += x[i][0] + x[i][1];
    sink = s; /* efeito colateral: impede o compilador de mover a chamada */
    return s;
}

static void bench_flops(void) {
    const long reps = 200000000;
    double v[3];
    for (int r = 0; r < 3; r++) {
        double t0 = now();
        sink = fma_kernel(reps);
        double t1 = now();
        v[r] = 2.0 * CH * reps / (t1 - t0) / 1e9;
    }
    printf("flops,1thread_gflops,%.3f\n", median(v, 3));
}

/* ---------- Matriz 1024x1024: ijk (ruim p/ cache) x ikj (amigável) ---------- */
static void bench_matmul(void) {
    const int N = 1024;
    double *A = malloc(sizeof(double) * N * N), *B = malloc(sizeof(double) * N * N),
           *C = malloc(sizeof(double) * N * N);
    for (int i = 0; i < N * N; i++) { A[i] = (xorshift() % 100) / 100.0; B[i] = (xorshift() % 100) / 100.0; }

    memset(C, 0, sizeof(double) * N * N);
    double t0 = now();
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double s = 0;
            for (int k = 0; k < N; k++) s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
    double t1 = now();
    printf("matmul,ijk_gflops,%.3f\n", 2.0 * N * N * N / (t1 - t0) / 1e9 + (C[7] == -1));

    memset(C, 0, sizeof(double) * N * N);
    t0 = now();
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double a = A[i * N + k];
            for (int j = 0; j < N; j++) C[i * N + j] += a * B[k * N + j];
        }
    t1 = now();
    printf("matmul,ikj_gflops,%.3f\n", 2.0 * N * N * N / (t1 - t0) / 1e9 + (C[7] == -1));
    free(A); free(B); free(C);
}

/* ---------- Eixo 6: paralelismo (escalonamento forte) ---------- */
typedef struct { int kind; long work; double *a, *b, *c; size_t n; } targ;

static void *worker(void *p) {
    targ *t = p;
    if (t->kind == 0) {
        volatile double s = fma_kernel(t->work);
        (void)s;
    } else {
        for (long r = 0; r < t->work; r++)
            for (size_t i = 0; i < t->n; i++) t->a[i] = t->b[i] + 3.0 * t->c[i];
    }
    return NULL;
}

static double run_threads(int kind, int T, long total, double *a, double *b, double *c, size_t n) {
    pthread_t th[256];
    targ args[256];
    double t0 = now();
    for (int i = 0; i < T; i++) {
        args[i].kind = kind;
        if (kind == 0) { args[i].work = total / T; }
        else {
            size_t chunk = n / T;
            args[i].work = total;
            args[i].a = a + i * chunk; args[i].b = b + i * chunk; args[i].c = c + i * chunk;
            args[i].n = chunk;
        }
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    return now() - t0;
}

static void bench_scale(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 256) ncpu = 256;
    size_t n = 16u << 20;
    double *a = malloc(n * 8), *b = malloc(n * 8), *c = malloc(n * 8);
    for (size_t i = 0; i < n; i++) { a[i] = 0; b[i] = 1; c[i] = 2; }
    printf("scale,ncpu,%d\n", ncpu);
    run_threads(1, ncpu, 2, a, b, c, n);              /* aquecimento: páginas e frequência */
    run_threads(0, 1, 50000000L, NULL, NULL, NULL, 0);
    for (int T = 1; T <= ncpu; T++) {
        printf("scale_cpu,%d,%.4f\n", T, run_threads(0, T, 400000000L, NULL, NULL, NULL, 0));
        printf("scale_mem,%d,%.4f\n", T, run_threads(1, T, 10, a, b, c, n));
        fflush(stdout);
    }
    free(a); free(b); free(c);
}

int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "all";
    int all = !strcmp(w, "all");
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (all || !strcmp(w, "cache"))  bench_cache();
    if (all || !strcmp(w, "branch")) bench_branch();
    if (all || !strcmp(w, "mem"))    bench_mem();
    if (all || !strcmp(w, "flops"))  bench_flops();
    if (all || !strcmp(w, "matmul")) bench_matmul();
    if (all || !strcmp(w, "scale"))  bench_scale();
    return 0;
}
