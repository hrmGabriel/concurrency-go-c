package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "hybrid" {
		if err := hybridCLI(os.Args[2:]); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	n := flag.Int("n", 5_000_000, "limite superior do intervalo [1, n]")
	workers := flag.Int("workers", runtime.NumCPU(), "número de goroutines (modo par)")
	mode := flag.String("mode", "bench", "modo de execução: seq | par | bench | sample")
	runs := flag.Int("runs", 3, "repetições para média de tempo")
	csvPath := flag.String("csv", "results.csv", "caminho do CSV no modo bench")
	idleWatts := flag.Float64("idle-watts", 25.0, "potência da CPU ociosa (W), usada quando o RAPL não está disponível")
	maxWatts := flag.Float64("max-watts", 105.0, "potência com todos os núcleos em carga, TDP (W), usada quando o RAPL não está disponível")
	requireRapl := flag.Bool("require-rapl", false, "interrompe em falha de leitura RAPL, sem estimar energia")
	flag.Parse()

	if *n < 1 || *n > 100_000_000 {
		fmt.Fprintln(os.Stderr, "erro: -n deve estar em [1, 100000000]")
		os.Exit(1)
	}
	if *runs < 1 {
		fmt.Fprintln(os.Stderr, "erro: -runs deve ser >= 1")
		os.Exit(1)
	}

	fmt.Printf("Ambiente: CPUs=%d GOMAXPROCS=%d Go=%s\n",
		runtime.NumCPU(), runtime.GOMAXPROCS(0), runtime.Version())
	fmt.Printf("Intervalo: [1, %d] | mode=%s | runs=%d\n", *n, *mode, *runs)

	meter := Meter{
		Model:       PowerModel{IdleWatts: *idleWatts, MaxWatts: *maxWatts},
		UseRapl:     raplAvailable(),
		NumCPU:      runtime.NumCPU(),
		RequireRapl: *requireRapl,
	}
	if *requireRapl && !meter.UseRapl {
		fmt.Fprintln(os.Stderr, "erro: RAPL obrigatório, contador sem acesso")
		os.Exit(1)
	}
	if meter.UseRapl {
		fmt.Println("Energia: rapl (medida)")
	} else {
		fmt.Printf("Energia: modelo (estimada) | ocioso=%.1f W máx=%.1f W\n", *idleWatts, *maxWatts)
	}

	switch *mode {
	case "sample":
		if *workers < 0 || *workers > 1024 {
			fmt.Fprintln(os.Stderr, "erro: -workers deve estar em [0, 1024]")
			os.Exit(1)
		}
		tp, joules, count := meter.measure(1, max(1, *workers), func() int {
			if *workers == 0 {
				return countPrimesSeq(1, *n)
			}
			return countPrimesPar(1, *n, *workers)
		})
		source := "modelo"
		if meter.UseRapl {
			source = "rapl"
		}
		data := fmt.Sprintf("workers,tempo_ms,primos,energia_j,fonte_energia\n%d,%.9f,%d,%.9f,%s\n",
			*workers, float64(tp)/1e6, count, joules, source)
		if err := os.WriteFile(*csvPath, []byte(data), 0644); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	case "seq":
		runSequential(meter, *n, *runs)
	case "par":
		runParallel(meter, *n, *workers, *runs)
	case "bench":
		runBench(meter, *n, *runs, *csvPath)
	default:
		fmt.Fprintf(os.Stderr, "erro: mode inválido %q (use seq, par ou bench)\n", *mode)
		os.Exit(1)
	}
}

func runSequential(meter Meter, n, runs int) {
	elapsed, joules, count := meter.measure(runs, 1, func() int {
		return countPrimesSeq(1, n)
	})
	fmt.Printf("Sequencial: primos=%d tempo_médio=%v energia_média=%.2f J\n", count, elapsed, joules)
}

func runParallel(meter Meter, n, workers, runs int) {
	if workers < 1 {
		fmt.Fprintln(os.Stderr, "erro: -workers deve ser >= 1")
		os.Exit(1)
	}
	elapsed, joules, count := meter.measure(runs, workers, func() int {
		return countPrimesPar(1, n, workers)
	})
	fmt.Printf("Paralelo (workers=%d): primos=%d tempo_médio=%v energia_média=%.2f J\n",
		workers, count, elapsed, joules)
}

func runBench(meter Meter, n, runs int, csvPath string) {
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
	seqTime, seqJoules, _ := meter.measure(runs, 1, func() int {
		return countPrimesSeq(1, n)
	})

	grid := workerGrid(runtime.NumCPU())
	results := make([]BenchmarkResult, 0, len(grid))

	for _, w := range grid {
		fmt.Printf("Medindo workers=%d...\n", w)
		tp, joules, count := meter.measure(runs, w, func() int {
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
			Joules:     joules,
			Greenup:    greenup(seqJoules, joules),
			EDP:        edp(joules, tp.Seconds()),
		})
	}

	printResultsTable(results, seqTime, seqJoules, seqCount)

	if err := writeResultsCSV(csvPath, results); err != nil {
		fmt.Fprintf(os.Stderr, "aviso: não foi possível gravar %s: %v\n", csvPath, err)
	} else {
		fmt.Printf("Resultados gravados em %s\n", csvPath)
	}
}
