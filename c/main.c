#include <math.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

// arquivo do contador de energia da cpu (linux, intel e amd)
#define RAPL_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"

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

// verifica se n eh primo
int isPrime(int n) {
    if (n < 2) {
        return 0;
    }

    if (n == 2) {
        return 1;
    }

    if (n % 2 == 0) {
        return 0;
    }

    int limit = (int)sqrt((double)n);

    for (int divisor = 3; divisor <= limit; divisor += 2) {
        if (n % divisor == 0) {
            return 0;
        }
    }

    return 1;
}

// conta primos em [start, end]
int countPrimesSeq(int start, int end) {
    int count = 0;

    for (int n = start; n <= end; n++) {
        if (isPrime(n)) {
            count++;
        }
    }

    return count;
}

// conta primos em paralelo
int countPrimesPar(int start, int end, int workers) {
    if (workers <= 1) {
        return countPrimesSeq(start, end);
    }

    int total = end - start + 1;

    if (workers > total) {
        workers = total;
    }

    int counts[workers];
    int segStarts[workers];
    int segEnds[workers];
    int base = total / workers;
    int extra = total % workers;
    int curr = start;

    // divide o intervalo
    for (int w = 0; w < workers; w++) {
        int size = base;

        if (w < extra) {
            size++;
        }

        segStarts[w] = curr;
        segEnds[w] = curr + size - 1;
        curr = segEnds[w] + 1;
    }

#pragma omp parallel for num_threads(workers)
    for (int w = 0; w < workers; w++) {
        counts[w] = countPrimesSeq(segStarts[w], segEnds[w]);
    }

    int sum = 0;

    for (int w = 0; w < workers; w++) {
        sum += counts[w];
    }

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
        double t0 = omp_get_wtime();

        *count = workers == 0 ? countPrimesSeq(start, end)
                              : countPrimesPar(start, end, workers);

        double seconds = omp_get_wtime() - t0;
        double energy = useRapl ? raplJoules() - j0 : -1;

        // sem rapl, ou o contador deu a volta: usa o modelo
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
