#include <math.h>
#include <omp.h>
#include <stdio.h>

typedef struct {
    int workers;
    double timeMs;
    int count;
    double speedup;
    double efficiency;
} BenchmarkResult;

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

    fprintf(file, "workers,tempo_ms,primos,speedup,eficiencia\n");

    for (int i = 0; i < size; i++) {
        fprintf(file, "%d,%.3f,%d,%.6f,%.6f\n",
                results[i].workers,
                results[i].timeMs,
                results[i].count,
                results[i].speedup,
                results[i].efficiency);
    }

    fclose(file);
}

int main(void) {
    int start = 1;
    int end = 5000000;
    int runs = 3;
    int count = 0;
    double totalTime = 0;

    // baseline sequencial
    for (int run = 0; run < runs; run++) {
        double startTime = omp_get_wtime();
        count = countPrimesSeq(start, end);
        totalTime += omp_get_wtime() - startTime;
    }

    double seqTime = totalTime / runs;
    int numCPU = omp_get_num_procs();
    int workers[6];
    int size = workerGrid(numCPU, workers);
    BenchmarkResult results[6];

    printf("cpus: %d\n", numCPU);
    printf("baseline sequencial: %.3f ms | primos: %d\n\n",
           seqTime * 1000.0, count);
    printf("workers  tempo_ms  primos  speedup  eficiencia\n");

    for (int i = 0; i < size; i++) {
        totalTime = 0;

        for (int run = 0; run < runs; run++) {
            double startTime = omp_get_wtime();
            count = countPrimesPar(start, end, workers[i]);
            totalTime += omp_get_wtime() - startTime;
        }

        double parTime = totalTime / runs;
        double speedup = seqTime / parTime;
        double efficiency = speedup / workers[i];

        results[i].workers = workers[i];
        results[i].timeMs = parTime * 1000.0;
        results[i].count = count;
        results[i].speedup = speedup;
        results[i].efficiency = efficiency;

        printf("%-8d %.3f    %d  %.3f    %.3f\n",
               workers[i], results[i].timeMs, count, speedup, efficiency);
    }

    writeResultsCSV("results.csv", results, size);

    return 0;
}
