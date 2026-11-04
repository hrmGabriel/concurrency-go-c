#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cpuid.h>
#include <errno.h>
#define main legacy_benchmark_main
#include "main.c"
#undef main

#define SEGMENT_BYTES 16384
#define MAX_HYBRID_WORKERS 1024

typedef struct {
    int worker, requested, start_cpu, end_cpu;
    long start_tid, end_tid;
    int singleton, restored;
} PinRecord;

int hybrid_base_primes(int32_t n, int32_t **out) {
    int32_t limit = (int32_t)sqrt((double)n);
    unsigned char *composite = calloc((size_t)limit + 1, 1);
    int32_t *primes = calloc((size_t)limit + 1, sizeof(*primes));
    if (!composite || !primes) { fputs("allocation failure\n", stderr); exit(1); }
    for (int32_t p = 2; p * p <= limit; p++)
        if (!composite[p])
            for (int32_t j = p * p; j <= limit; j += p) composite[j] = 1;
    int count = 0;
    for (int32_t p = 2; p <= limit; p++) if (!composite[p]) primes[count++] = p;
    free(composite);
    *out = primes;
    return count;
}

int32_t hybrid_segmented(int32_t start, int32_t end, const int32_t *primes, int base_count,
                         unsigned char *composite) {
    int32_t count = 0;
    for (int32_t low = start; low <= end; low += SEGMENT_BYTES) {
        int32_t high = low + SEGMENT_BYTES - 1;
        if (high > end) high = end;
        memset(composite, 0, (size_t)(high - low + 1));
        for (int i = 0; i < base_count; i++) {
            int32_t p = primes[i];
            int32_t first = ((low + p - 1) / p) * p;
            if (first < p * p) first = p * p;
            for (int32_t j = first; j <= high; j += p) composite[j - low] = 1;
        }
        for (int32_t value = low; value <= high; value++)
            if (value >= 2 && !composite[value - low]) count++;
    }
    return count;
}

int hybrid_worker(PrimeRange *range, int id, int cpu, int segmented,
                  const int32_t *primes, int base_count, unsigned char *buffer, PinRecord *record) {
    cpu_set_t original, pinned, observed, restored;
    record->worker = id; record->requested = cpu;
    if (sched_getaffinity(0, sizeof(original), &original)) return 1;
    CPU_ZERO(&pinned); CPU_SET(cpu, &pinned);
    if (sched_setaffinity(0, sizeof(pinned), &pinned)) return 2;
    int error = 0;
    if (sched_getaffinity(0, sizeof(observed), &observed)) error = 3;
    record->singleton = !error && CPU_COUNT(&observed) == 1 && CPU_ISSET(cpu, &observed);
    record->start_cpu = sched_getcpu(); record->start_tid = syscall(SYS_gettid);
    range->count = segmented ? hybrid_segmented(range->start, range->end, primes, base_count, buffer)
                             : countPrimesSeq(range->start, range->end);
    record->end_cpu = sched_getcpu(); record->end_tid = syscall(SYS_gettid);
    if (!record->singleton || record->start_cpu != cpu || record->end_cpu != cpu ||
        record->start_tid != record->end_tid) error = 4;
    if (sched_setaffinity(0, sizeof(original), &original)) return 5;
    record->restored = sched_getaffinity(0, sizeof(restored), &restored) == 0 && CPU_EQUAL(&original, &restored);
    return record->restored ? error : 6;
}

int hybrid_once(int32_t n, int workers, const int *cpus, int segmented, PinRecord *records) {
    int effective = workers ? workers : 1;
    PrimeRange *ranges = calloc((size_t)effective, sizeof(*ranges));
    int *errors = calloc((size_t)effective, sizeof(*errors));
    if (!ranges || !errors) { fputs("allocation failure\n", stderr); exit(1); }
    int32_t base = n / effective, extra = n % effective, current = 1;
    for (int32_t w = 0; w < effective; w++) {
        int32_t size = base + (w < extra);
        ranges[w].start = current; ranges[w].end = current + size - 1;
        current = ranges[w].end + 1;
    }
    int32_t *primes = NULL;
    int base_count = segmented ? hybrid_base_primes(n, &primes) : 0;
    unsigned char *buffers = segmented ? calloc((size_t)effective, SEGMENT_BYTES) : NULL;
    if (segmented && !buffers) { fputs("allocation failure\n", stderr); exit(1); }
    if (!workers) {
        errors[0] = hybrid_worker(&ranges[0], 0, cpus[0], segmented, primes, base_count, buffers, records);
    } else {
#pragma omp parallel num_threads(effective)
        {
            int w = omp_get_thread_num();
            if (omp_get_num_threads() != effective) errors[w] = 7;
            else errors[w] = hybrid_worker(&ranges[w], w, cpus[w], segmented, primes, base_count,
                                          segmented ? buffers + w * SEGMENT_BYTES : NULL, &records[w]);
        }
    }
    int count = 0;
    for (int w = 0; w < effective; w++) {
        if (errors[w]) { fprintf(stderr, "affinity failure worker=%d code=%d\n", w, errors[w]); exit(1); }
        count += ranges[w].count;
    }
    free(buffers); free(primes); free(ranges); free(errors);
    return count;
}

#ifndef HYBRID_LIBRARY
int hybrid_topology(void) {
    cpu_set_t original, one;
    if (sched_getaffinity(0, sizeof(original), &original)) return 1;
    printf("["); int first = 1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, &original)) {
        CPU_ZERO(&one); CPU_SET(cpu, &one);
        if (sched_setaffinity(0, sizeof(one), &one)) return 1;
        unsigned a = 0, b, c, d;
        if (__get_cpuid_max(0, NULL) >= 0x1a) __cpuid_count(0x1a, 0, a, b, c, d);
        printf("%s{\"cpu\":%d,\"cpuid_core_type\":%u}", first ? "" : ",", cpu, a >> 24);
        first = 0;
    }
    if (sched_setaffinity(0, sizeof(original), &original)) return 1;
    puts("]"); return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--topology")) return hybrid_topology();
    if (argc < 6 || argc > 7) { fputs("usage: binary trial|segmented N workers batch cpu,cpu [--check]\n", stderr); return 1; }
    int segmented = !strcmp(argv[1], "segmented");
    if (!segmented && strcmp(argv[1], "trial")) return 1;
    int n = atoi(argv[2]), workers = atoi(argv[3]), batch = atoi(argv[4]);
    int effective = workers ? workers : 1;
    if (n < 1 || n > 100000000 || workers < 0 || effective > MAX_HYBRID_WORKERS ||
        effective > n || batch < 1 || batch > 10000) return 1;
    int cpus[MAX_HYBRID_WORKERS], cpu_count = 0;
    char *list = strdup(argv[5]);
    if (!list) return 1;
    for (char *p = strtok(list, ","); p; p = strtok(NULL, ",")) {
        char *end; long cpu = strtol(p, &end, 10);
        if (*end || cpu < 0 || cpu >= CPU_SETSIZE || cpu_count >= MAX_HYBRID_WORKERS) return 1;
        for (int j = 0; j < cpu_count; j++) if (cpus[j] == cpu) return 1;
        cpus[cpu_count++] = (int)cpu;
    }
    free(list);
    if (cpu_count != effective) return 1;
    int check = argc == 7 && !strcmp(argv[6], "--check");
    if (argc == 7 && !check) return 1;
    PinRecord *records = calloc((size_t)batch * effective, sizeof(*records));
    if (!records) return 1;
    double range = raplRangeJoules(), j0 = check ? 0 : raplJoules();
    if (!check && (j0 < 0 || range <= 0)) { fputs("RAPL required\n", stderr); return 1; }
    int count = -1;
    double start = omp_get_wtime();
    for (int i = 0; i < batch; i++) {
        int value = hybrid_once(n, workers, cpus, segmented, records + i * effective);
        if (count >= 0 && value != count) return 1;
        count = value;
    }
    double seconds = omp_get_wtime() - start;
    double j1 = check ? 0 : raplJoules();
    double energy = check ? 0 : raplDelta(j0, j1, range);
    if (!check && energy <= 0) { fputs("invalid RAPL energy\n", stderr); return 1; }
    printf("{\"algorithm\":\"%s\",\"n\":%d,\"workers\":%d,\"batch\":%d,\"count\":%d,\"time_ms\":%.9f,\"energy_j\":%.9f,\"batch_time_ms\":%.9f,\"source_energy\":\"%s\",\"placements\":[",
           argv[1], n, workers, batch, count, seconds * 1000 / batch, energy / batch,
           seconds * 1000, check ? "check" : "rapl");
    for (int i = 0; i < batch * effective; i++) {
        PinRecord *r = &records[i];
        printf("%s{\"worker\":%d,\"requested\":%d,\"start_cpu\":%d,\"end_cpu\":%d,\"start_tid\":%ld,\"end_tid\":%ld,\"singleton\":%d,\"restored\":%d}", i ? "," : "", r->worker, r->requested, r->start_cpu, r->end_cpu, r->start_tid, r->end_tid, r->singleton, r->restored);
    }
    puts("]}"); free(records); return 0;
}
#endif
