package main

import (
	"encoding/csv"
	"fmt"
	"os"
	"strconv"
	"time"
)

// BenchmarkResult guarda o resultado de uma configuração de workers.
type BenchmarkResult struct {
	Workers    int
	Time       time.Duration
	Count      int
	Speedup    float64
	Efficiency float64
}

// averageDuration executa fn runs vezes e retorna a duração média e o último resultado.
func averageDuration(runs int, fn func() int) (time.Duration, int) {
	if runs < 1 {
		runs = 1
	}
	var total time.Duration
	var last int
	for i := 0; i < runs; i++ {
		start := time.Now()
		last = fn()
		total += time.Since(start)
	}
	return total / time.Duration(runs), last
}

// speedup calcula S = T1 / Tp.
func speedup(t1, tp time.Duration) float64 {
	if tp <= 0 {
		return 0
	}
	return float64(t1) / float64(tp)
}

// efficiency calcula E = S / W.
func efficiency(s float64, workers int) float64 {
	if workers <= 0 {
		return 0
	}
	return s / float64(workers)
}

// workerGrid monta a grade de workers alinhada ao hardware.
func workerGrid(numCPU int) []int {
	candidates := []int{1, 2, 4, 8, numCPU, 2 * numCPU}
	seen := make(map[int]bool)
	grid := make([]int, 0, len(candidates))
	for _, w := range candidates {
		if w < 1 {
			continue
		}
		if !seen[w] {
			seen[w] = true
			grid = append(grid, w)
		}
	}
	return grid
}

// printResultsTable imprime a tabela de resultados no stdout.
func printResultsTable(results []BenchmarkResult, seqTime time.Duration, seqCount int) {
	fmt.Println()
	fmt.Printf("Baseline sequencial: %v | primos: %d\n", seqTime, seqCount)
	fmt.Println()
	fmt.Printf("%-10s %-14s %-10s %-12s %-12s\n", "Workers", "Tempo", "Primos", "Speedup", "Eficiência")
	fmt.Printf("%-10s %-14s %-10s %-12s %-12s\n", "-------", "-----", "------", "-------", "----------")
	for _, r := range results {
		fmt.Printf("%-10d %-14v %-10d %-12.4f %-12.4f\n",
			r.Workers, r.Time, r.Count, r.Speedup, r.Efficiency)
	}
	fmt.Println()
}

// writeResultsCSV grava results.csv com as métricas do experimento.
func writeResultsCSV(path string, results []BenchmarkResult) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	w := csv.NewWriter(f)
	defer w.Flush()

	if err := w.Write([]string{"workers", "tempo_ms", "primos", "speedup", "eficiencia"}); err != nil {
		return err
	}
	for _, r := range results {
		row := []string{
			strconv.Itoa(r.Workers),
			strconv.FormatFloat(float64(r.Time.Microseconds())/1000.0, 'f', 3, 64),
			strconv.Itoa(r.Count),
			strconv.FormatFloat(r.Speedup, 'f', 6, 64),
			strconv.FormatFloat(r.Efficiency, 'f', 6, 64),
		}
		if err := w.Write(row); err != nil {
			return err
		}
	}
	return w.Error()
}
