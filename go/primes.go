package main

import (
	"math"
	"sync"
)

// Candidatos, divisores, limites e contagens têm 32 bits, como em C.
func isPrime(n int32) bool {
	if n < 2 {
		return false
	}
	if n == 2 {
		return true
	}
	if n%2 == 0 {
		return false
	}
	limit := int32(math.Sqrt(float64(n)))
	for divisor := int32(3); divisor <= limit; divisor += 2 {
		if n%divisor == 0 {
			return false
		}
	}
	return true
}

func countPrimesRange(start, end int32) int32 {
	var count int32
	for n := start; n <= end; n++ {
		if isPrime(n) {
			count++
		}
	}
	return count
}

// A interface com CLI/métricas usa int; o núcleo aritmético usa int32.
func countPrimesSeq(start, end int) int {
	if end < start {
		return 0
	}
	return int(countPrimesRange(int32(start), int32(end)))
}

// Mesmo registro de 12 bytes usado em C, na mesma ordem dos campos.
type primeRange struct{ start, end, count int32 }

func countPrimesPar(start, end, workers int) int {
	if end < start {
		return 0
	}
	if workers < 1 {
		return countPrimesSeq(start, end)
	}
	total := int32(end - start + 1)
	if workers > int(total) {
		workers = int(total)
	}
	ranges := make([]primeRange, workers)
	base := total / int32(workers)
	extra := total % int32(workers)
	curr := int32(start)
	// Todas as faixas são preparadas antes de lançar qualquer goroutine.
	for w := int32(0); w < int32(workers); w++ {
		size := base
		if w < extra {
			size++
		}
		ranges[w].start = curr
		ranges[w].end = curr + size - 1
		curr = ranges[w].end + 1
	}
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := int32(0); w < int32(workers); w++ {
		go func(idx int32) {
			ranges[idx].count = countPrimesRange(ranges[idx].start, ranges[idx].end)
			wg.Done()
		}(w)
	}
	wg.Wait()
	var sum int32
	for w := int32(0); w < int32(workers); w++ {
		sum += ranges[w].count
	}
	return int(sum)
}
