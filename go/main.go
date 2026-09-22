package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
)

func main() {
	n := flag.Int("n", 5_000_000, "limite superior do intervalo [1, n]")
	workers := flag.Int("workers", runtime.NumCPU(), "número de goroutines (modo par)")
	mode := flag.String("mode", "bench", "modo de execução: seq | par | bench")
	runs := flag.Int("runs", 3, "repetições para média de tempo")
	csvPath := flag.String("csv", "results.csv", "caminho do CSV no modo bench")
	flag.Parse()

	if *n < 1 {
		fmt.Fprintln(os.Stderr, "erro: -n deve ser >= 1")
		os.Exit(1)
	}
	if *runs < 1 {
		fmt.Fprintln(os.Stderr, "erro: -runs deve ser >= 1")
		os.Exit(1)
	}

	fmt.Printf("Ambiente: CPUs=%d GOMAXPROCS=%d Go=%s\n",
		runtime.NumCPU(), runtime.GOMAXPROCS(0), runtime.Version())
	fmt.Printf("Intervalo: [1, %d] | mode=%s | runs=%d\n", *n, *mode, *runs)

	switch *mode {
	case "seq":
		runSequential(*n, *runs)
	case "par":
		runParallel(*n, *workers, *runs)
	case "bench":
		runBench(*n, *runs, *csvPath)
	default:
		fmt.Fprintf(os.Stderr, "erro: mode inválido %q (use seq, par ou bench)\n", *mode)
		os.Exit(1)
	}
}

func runSequential(n, runs int) {
	elapsed, count := averageDuration(runs, func() int {
		return countPrimesSeq(1, n)
	})
	fmt.Printf("Sequencial: primos=%d tempo_médio=%v\n", count, elapsed)
}

func runParallel(n, workers, runs int) {
	if workers < 1 {
		fmt.Fprintln(os.Stderr, "erro: -workers deve ser >= 1")
		os.Exit(1)
	}
	elapsed, count := averageDuration(runs, func() int {
		return countPrimesPar(1, n, workers)
	})
	fmt.Printf("Paralelo (workers=%d): primos=%d tempo_médio=%v\n", workers, count, elapsed)
}

func runBench(n, runs int, csvPath string) {
	fmt.Println("Validando corretude (seq vs par)...")
	seqCount := countPrimesSeq(1, n)
	for _, w := range []int{1, 2, 4, runtime.NumCPU()} {
		parCount := countPrimesPar(1, n, w)
		if parCount != seqCount {
			fmt.Fprintf(os.Stderr,
				"erro de corretude: seq=%d par(workers=%d)=%d\n", seqCount, w, parCount)
			os.Exit(1)
		}
	}
	fmt.Printf("Corretude OK: %d primos em [1, %d]\n", seqCount, n)

	fmt.Println("Medindo baseline sequencial...")
	seqTime, _ := averageDuration(runs, func() int {
		return countPrimesSeq(1, n)
	})

	grid := workerGrid(runtime.NumCPU())
	results := make([]BenchmarkResult, 0, len(grid))

	for _, w := range grid {
		fmt.Printf("Medindo workers=%d...\n", w)
		tp, count := averageDuration(runs, func() int {
			return countPrimesPar(1, n, w)
		})
		s := speedup(seqTime, tp)
		e := efficiency(s, w)
		results = append(results, BenchmarkResult{
			Workers:    w,
			Time:       tp,
			Count:      count,
			Speedup:    s,
			Efficiency: e,
		})
	}

	printResultsTable(results, seqTime, seqCount)

	if err := writeResultsCSV(csvPath, results); err != nil {
		fmt.Fprintf(os.Stderr, "aviso: não foi possível gravar %s: %v\n", csvPath, err)
	} else {
		fmt.Printf("Resultados gravados em %s\n", csvPath)
	}
}
