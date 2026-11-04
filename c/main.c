#include <math.h>
#include <stdint.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// arquivo do contador de energia da cpu (linux, intel e amd)
#define RAPL_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define RAPL_RANGE_PATH "/sys/class/powercap/intel-rapl:0/max_energy_range_uj"

typedef struct {
    int workers;
    double timeMs;
    int count;
    double speedup;
    double efficiency;
    double energyJ;   // energia consumida (J)
    double greenup;   // E_seq / E_p
    double edp;       // energy-delay product (J*s)
} BenchmarkResult;

// modelo de potencia, usado quando o rapl nao esta disponivel (ex.: windows)
double idleWatts = 25.0;   // cpu ligada e ociosa
double maxWatts = 105.0;   // todos os nucleos em carga (tdp)
int useRapl = 0;
int requireRapl = 0;

// verifica se n eh primo
int isPrime(int32_t n) {
    if (n < 2) {
        return 0;
    }

    if (n == 2) {
        return 1;
    }

    if (n % 2 == 0) {
        return 0;
    }

    int32_t limit = (int32_t)sqrt((double)n);

    for (int32_t divisor = 3; divisor <= limit; divisor += 2) {
        if (n % divisor == 0) {
            return 0;
        }
    }

    return 1;
}

// conta primos em [start, end]
int countPrimesSeq(int start, int end) {
    int32_t count = 0;

    for (int32_t n = start; n <= end; n++) {
        if (isPrime(n)) {
            count++;
        }
    }

    return count;
}

// Mesmo registro de 12 bytes usado em Go: limites e contagem em int32.
typedef struct {
    int32_t start;
    int32_t end;
    int32_t count;
} PrimeRange;
_Static_assert(sizeof(PrimeRange) == 12, "PrimeRange deve ocupar 12 bytes");

// Conta primos com partição integralmente preparada antes de iniciar workers.
int countPrimesPar(int start, int end, int workers) {
    if (end < start) return 0;
    if (workers < 1) return countPrimesSeq(start, end);
    int32_t total = end - start + 1;
    if (workers > total) workers = total;
    PrimeRange *ranges = calloc((size_t)workers, sizeof(*ranges));
    if (ranges == NULL) {
        fputs("erro: alocação das faixas falhou\n", stderr);
        exit(1);
    }
    int32_t base = total / workers;
    int32_t extra = total % workers;
    int32_t curr = start;
    for (int32_t w = 0; w < workers; w++) {
        int32_t size = base + (w < extra);
        ranges[w].start = curr;
        ranges[w].end = curr + size - 1;
        curr = ranges[w].end + 1;
    }
#pragma omp parallel for num_threads(workers) schedule(static)
    for (int32_t w = 0; w < workers; w++) {
        ranges[w].count = countPrimesSeq(ranges[w].start, ranges[w].end);
    }
    int32_t sum = 0;
    for (int32_t w = 0; w < workers; w++) sum += ranges[w].count;
    free(ranges);
    return sum;
}

// monta a lista de workers
int workerGrid(int numCPU, int workers[]) {
    int candidates[] = {1, 2, 4, 8, numCPU, 2 * numCPU};
    int size = 0;

    for (int i = 0; i < 6; i++) {
        int repeated = 0;

        for (int j = 0; j < size; j++) {
            if (workers[j] == candidates[i]) {
                repeated = 1;
            }
        }

        if (repeated == 0) {
            workers[size] = candidates[i];
            size++;
        }
    }

    return size;
}

// salva os resultados
void writeResultsCSV(const char *path, BenchmarkResult results[], int size) {
    FILE *file = fopen(path, "w");

    fprintf(file, "workers,tempo_ms,primos,speedup,eficiencia,energia_j,greenup,edp_js\n");

    for (int i = 0; i < size; i++) {
        fprintf(file, "%d,%.3f,%d,%.6f,%.6f,%.4f,%.4f,%.4f\n",
                results[i].workers,
                results[i].timeMs,
                results[i].count,
                results[i].speedup,
                results[i].efficiency,
                results[i].energyJ,
                results[i].greenup,
                results[i].edp);
    }

    fclose(file);
}

// le o contador rapl em joules, -1 se indisponivel
double raplJoules(void) {
    FILE *file = fopen(RAPL_PATH, "r");

    if (file == NULL) {
        return -1;
    }

    unsigned long long uj = 0;
    int ok = fscanf(file, "%llu", &uj);
    fclose(file);

    return ok == 1 ? uj / 1e6 : -1;
}

// Trata uma volta do contador; leituras inválidas nunca viram energia válida.
double raplDelta(double j0, double j1, double range) {
    if (j0 < 0 || j1 < 0) return -1;
    if (j1 >= j0) return j1 - j0;
    if (range <= j0 || range <= j1) return -1;
    return j1 - j0 + range;
}

double raplRangeJoules(void) {
    FILE *file = fopen(RAPL_RANGE_PATH, "r");
    if (file == NULL) return -1;
    unsigned long long uj = 0;
    int ok = fscanf(file, "%llu", &uj);
    fclose(file);
    return ok == 1 ? uj / 1e6 : -1;
}

// energia estimada: parte fixa + parte por cpu ativa
double modelJoules(double seconds, int workers, int numCPU) {
    int active = workers < numCPU ? workers : numCPU;
    double wattsPerCPU = (maxWatts - idleWatts) / numCPU;

    return (idleWatts + wattsPerCPU * active) * seconds;
}

// roda a contagem runs vezes (workers = 0: sequencial)
// devolve o tempo medio; a energia media vai em *joules
double measure(int start, int end, int workers, int runs,
               int numCPU, int *count, double *joules) {
    double totalTime = 0;
    double totalJoules = 0;

    for (int run = 0; run < runs; run++) {
        double j0 = useRapl ? raplJoules() : 0;
        if (requireRapl && (!useRapl || j0 < 0)) {
            fprintf(stderr, "erro: RAPL obrigatório, leitura inicial indisponível\n");
            exit(1);
        }
        double t0 = omp_get_wtime();

        *count = workers == 0 ? countPrimesSeq(start, end)
                              : countPrimesPar(start, end, workers);

        double seconds = omp_get_wtime() - t0;
        double energy = -1;
        if (useRapl) {
            double j1 = raplJoules();
            energy = raplDelta(j0, j1, j1 < j0 ? raplRangeJoules() : 0);
        }
        if (requireRapl && energy <= 0) {
            fprintf(stderr, "erro: RAPL obrigatório, delta de energia inválido\n");
            exit(1);
        }

        // Modelo somente no modo que permite estimativas.
        if (energy < 0) {
            energy = modelJoules(seconds, workers == 0 ? 1 : workers, numCPU);
        }

        totalTime += seconds;
        totalJoules += energy;
    }

    *joules = totalJoules / runs;

    return totalTime / runs;
}

// uso: ./main [watts_ocioso] [watts_max]
int main(int argc, char *argv[]) {
    int start = 1;
    int end = 5000000;
    int runs = 3;
    int count = 0;
    int numCPU = omp_get_num_procs();

    // Uma medição individual para o protocolo reproduzível em Python.
    // workers=0 usa o baseline sequencial; o algoritmo permanece o mesmo.
    if (argc == 5 && (strcmp(argv[1], "--sample") == 0 ||
                      strcmp(argv[1], "--sample-rapl") == 0)) {
        int n = atoi(argv[2]);
        int w = atoi(argv[3]);
        if (n < 1 || n > 100000000 || w < 0 || w > 1024) return 1;
        useRapl = raplJoules() >= 0;
        requireRapl = strcmp(argv[1], "--sample-rapl") == 0;
        double energy;
        double seconds = measure(1, n, w, 1, numCPU, &count, &energy);
        FILE *file = fopen(argv[4], "w");
        if (file == NULL) { perror(argv[4]); return 1; }
        fprintf(file, "workers,tempo_ms,primos,energia_j,fonte_energia\n");
        fprintf(file, "%d,%.9f,%d,%.9f,%s\n", w, seconds * 1000,
                count, energy, useRapl ? "rapl" : "modelo");
        if (fclose(file) != 0) return 1;
        return 0;
    }

    if (argc > 1) idleWatts = atof(argv[1]);
    if (argc > 2) maxWatts = atof(argv[2]);
    useRapl = raplJoules() >= 0;

    // baseline sequencial
    double seqJoules;
    double seqTime = measure(start, end, 0, runs, numCPU, &count, &seqJoules);

    int workers[6];
    int size = workerGrid(numCPU, workers);
    BenchmarkResult results[6];

    printf("cpus: %d | energia: %s\n", numCPU, useRapl ? "rapl (medida)" : "modelo (estimada)");
    printf("baseline sequencial: %.3f ms | %.2f J | primos: %d\n\n",
           seqTime * 1000.0, seqJoules, count);
    printf("workers  tempo_ms  primos  speedup  eficiencia  energia_J  greenup  edp\n");

    for (int i = 0; i < size; i++) {
        double parJoules;
        double parTime = measure(start, end, workers[i], runs, numCPU, &count, &parJoules);
        double speedup = seqTime / parTime;
        double efficiency = speedup / workers[i];

        results[i].workers = workers[i];
        results[i].timeMs = parTime * 1000.0;
        results[i].count = count;
        results[i].speedup = speedup;
        results[i].efficiency = efficiency;
        results[i].energyJ = parJoules;
        results[i].greenup = seqJoules / parJoules;
        results[i].edp = parJoules * parTime;

        printf("%-8d %.3f    %d  %.3f    %.3f       %.2f       %.3f    %.3f\n",
               workers[i], results[i].timeMs, count, speedup, efficiency,
               results[i].energyJ, results[i].greenup, results[i].edp);
    }

    writeResultsCSV("results.csv", results, size);

    return 0;
}
