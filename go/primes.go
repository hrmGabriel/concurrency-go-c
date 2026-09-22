package main

import (
	"math"
	"sync"
)

// isPrime verifica se n é primo por divisão trial otimizada.
func isPrime(n int) bool {
	if n < 2 {
		return false
	}
	if n == 2 {
		return true
	}
	if n%2 == 0 {
		return false
	}
	limit := int(math.Sqrt(float64(n)))
	for i := 3; i <= limit; i += 2 {
		if n%i == 0 {
			return false
		}
	}
	return true
}

// countPrimesSeq conta primos no intervalo fechado [start, end].
func countPrimesSeq(start, end int) int {
	if end < start {
		return 0
	}
	count := 0
	for n := start; n <= end; n++ {
		if isPrime(n) {
			count++
		}
	}
	return count
}

// countPrimesPar conta primos em [start, end] usando workers goroutines
// com partição estática do intervalo e WaitGroup + slice por índice.
func countPrimesPar(start, end, workers int) int {
	if end < start {
		return 0
	}
	if workers <= 1 {
		return countPrimesSeq(start, end)
	}

	total := end - start + 1
	if workers > total {
		workers = total
	}

	counts := make([]int, workers)
	var wg sync.WaitGroup
	wg.Add(workers)

	base := total / workers
	extra := total % workers
	curr := start

	for w := 0; w < workers; w++ {
		size := base
		if w < extra {
			size++
		}
		segStart := curr
		segEnd := curr + size - 1
		curr = segEnd + 1

		go func(idx, s, e int) {
			defer wg.Done()
			counts[idx] = countPrimesSeq(s, e)
		}(w, segStart, segEnd)
	}

	wg.Wait()

	sum := 0
	for _, c := range counts {
		sum += c
	}
	return sum
}
