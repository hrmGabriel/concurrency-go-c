//go:build linux && amd64

package main

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

const hybridSegmentBytes = 16384

type pinRecord struct {
	Worker    int `json:"worker"`
	Requested int `json:"requested"`
	StartCPU  int `json:"start_cpu"`
	EndCPU    int `json:"end_cpu"`
	StartTID  int `json:"start_tid"`
	EndTID    int `json:"end_tid"`
	Singleton int `json:"singleton"`
	Restored  int `json:"restored"`
}

type cpuMask [128]byte // CPU_SETSIZE = 1024 on this Linux host.

func affinityGet(mask *cpuMask) error {
	_, _, err := syscall.RawSyscall(syscall.SYS_SCHED_GETAFFINITY, 0, uintptr(len(mask)), uintptr(unsafe.Pointer(mask)))
	if err != 0 {
		return err
	}
	return nil
}

func affinitySet(mask *cpuMask) error {
	_, _, err := syscall.RawSyscall(syscall.SYS_SCHED_SETAFFINITY, 0, uintptr(len(mask)), uintptr(unsafe.Pointer(mask)))
	if err != 0 {
		return err
	}
	return nil
}

func currentCPU() (int, error) {
	var cpu uint32
	// __NR_getcpu = 309 in the Linux x86-64 syscall ABI (build tag above).
	_, _, err := syscall.RawSyscall(309, uintptr(unsafe.Pointer(&cpu)), 0, 0)
	return int(cpu), func() error {
		if err != 0 {
			return err
		}
		return nil
	}()
}

func hybridBasePrimes(n int32) []int32 {
	limit := int32(math.Sqrt(float64(n)))
	composite := make([]byte, limit+1)
	primes := make([]int32, limit+1)
	for p := int32(2); p*p <= limit; p++ {
		if composite[p] == 0 {
			for j := p * p; j <= limit; j += p {
				composite[j] = 1
			}
		}
	}
	count := 0
	for p := int32(2); p <= limit; p++ {
		if composite[p] == 0 {
			primes[count] = p
			count++
		}
	}
	return primes[:count]
}

func hybridSegmented(start, end int32, primes []int32, composite []byte) int32 {
	var count int32
	for low := start; low <= end; low += hybridSegmentBytes {
		high := low + hybridSegmentBytes - 1
		if high > end {
			high = end
		}
		clear(composite[:high-low+1])
		for _, p := range primes {
			first := ((low + p - 1) / p) * p
			if first < p*p {
				first = p * p
			}
			for j := first; j <= high; j += p {
				composite[j-low] = 1
			}
		}
		for value := low; value <= high; value++ {
			if value >= 2 && composite[value-low] == 0 {
				count++
			}
		}
	}
	return count
}

func hybridWorker(r *primeRange, id, cpu int, segmented bool, primes []int32, buffer []byte, record *pinRecord) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	record.Worker, record.Requested = id, cpu
	var original, pinned, observed cpuMask
	if err := affinityGet(&original); err != nil {
		return err
	}
	pinned[cpu/8] |= 1 << (cpu % 8)
	if err := affinitySet(&pinned); err != nil {
		return err
	}
	// Restore before UnlockOSThread so this runtime thread can be reused safely.
	var workErr error
	if err := affinityGet(&observed); err != nil {
		workErr = err
	}
	if observed == pinned {
		record.Singleton = 1
	}
	var err error
	record.StartCPU, err = currentCPU()
	if err != nil {
		workErr = err
	}
	record.StartTID = syscall.Gettid()
	if segmented {
		r.count = hybridSegmented(r.start, r.end, primes, buffer)
	} else {
		r.count = countPrimesRange(r.start, r.end)
	}
	record.EndCPU, err = currentCPU()
	if err != nil {
		workErr = err
	}
	record.EndTID = syscall.Gettid()
	if record.Singleton != 1 || record.StartCPU != cpu || record.EndCPU != cpu || record.StartTID != record.EndTID {
		workErr = fmt.Errorf("affinity mismatch worker=%d", id)
	}
	if err := affinitySet(&original); err != nil {
		return err
	}
	if err := affinityGet(&observed); err != nil {
		return err
	}
	if observed != original {
		return fmt.Errorf("affinity not restored worker=%d", id)
	}
	record.Restored = 1
	return workErr
}

func hybridOnce(n int32, workers int, cpus []int, segmented bool, records []pinRecord) (int32, error) {
	effective := max(1, workers)
	ranges := make([]primeRange, effective)
	errors := make([]error, effective)
	base, extra, current := n/int32(effective), n%int32(effective), int32(1)
	for w := 0; w < effective; w++ {
		size := base
		if int32(w) < extra {
			size++
		}
		ranges[w].start, ranges[w].end = current, current+size-1
		current = ranges[w].end + 1
	}
	var primes []int32
	var buffers []byte
	if segmented {
		primes = hybridBasePrimes(n)
		buffers = make([]byte, effective*hybridSegmentBytes)
	}
	work := func(w int) {
		var buffer []byte
		if segmented {
			buffer = buffers[w*hybridSegmentBytes : (w+1)*hybridSegmentBytes]
		}
		errors[w] = hybridWorker(&ranges[w], w, cpus[w], segmented, primes, buffer, &records[w])
	}
	if workers == 0 {
		work(0)
	} else {
		var wg sync.WaitGroup
		wg.Add(workers)
		for w := 0; w < workers; w++ {
			go func(id int) { defer wg.Done(); work(id) }(w)
		}
		wg.Wait()
	}
	var count int32
	for w, r := range ranges {
		if errors[w] != nil {
			return 0, errors[w]
		}
		count += r.count
	}
	return count, nil
}

func hybridCLI(args []string) error {
	if len(args) < 5 || len(args) > 6 {
		return fmt.Errorf("usage: hybrid trial|segmented N workers batch cpu,cpu [--check]")
	}
	segmented := args[0] == "segmented"
	if !segmented && args[0] != "trial" {
		return fmt.Errorf("invalid algorithm")
	}
	values := make([]int, 3)
	for i := range values {
		v, err := strconv.Atoi(args[i+1])
		if err != nil {
			return err
		}
		values[i] = v
	}
	n, workers, batch := values[0], values[1], values[2]
	effective := max(1, workers)
	if n < 1 || n > 100_000_000 || workers < 0 || effective > 1024 || effective > n || batch < 1 || batch > 10000 {
		return fmt.Errorf("invalid limits")
	}
	var cpus []int
	seen := map[int]bool{}
	for _, value := range strings.Split(args[4], ",") {
		cpu, err := strconv.Atoi(value)
		if err != nil || cpu < 0 || cpu >= 1024 || seen[cpu] {
			return fmt.Errorf("invalid CPU list")
		}
		seen[cpu] = true
		cpus = append(cpus, cpu)
	}
	if len(cpus) != effective {
		return fmt.Errorf("CPU count differs from workers")
	}
	check := len(args) == 6 && args[5] == "--check"
	if len(args) == 6 && !check {
		return fmt.Errorf("invalid option")
	}
	records := make([]pinRecord, batch*effective)
	counterRange, rangeOK := readJoules(raplRangePath)
	j0, startOK := 0.0, true
	if !check {
		j0, startOK = raplJoules()
	}
	if !check && (!startOK || !rangeOK || counterRange <= 0) {
		return fmt.Errorf("RAPL required")
	}
	var count int32 = -1
	start := time.Now()
	for i := 0; i < batch; i++ {
		value, err := hybridOnce(int32(n), workers, cpus, segmented, records[i*effective:(i+1)*effective])
		if err != nil {
			return err
		}
		if count >= 0 && count != value {
			return fmt.Errorf("inconsistent count")
		}
		count = value
	}
	seconds := time.Since(start).Seconds()
	energy, source := 0.0, "check"
	if !check {
		j1, ok := raplJoules()
		if !ok {
			return fmt.Errorf("RAPL read failed")
		}
		energy, ok = raplDelta(j0, j1, counterRange)
		if !ok || energy <= 0 {
			return fmt.Errorf("invalid RAPL energy")
		}
		source = "rapl"
	}
	return json.NewEncoder(os.Stdout).Encode(struct {
		Algorithm    string      `json:"algorithm"`
		N            int         `json:"n"`
		Workers      int         `json:"workers"`
		Batch        int         `json:"batch"`
		Count        int32       `json:"count"`
		TimeMS       float64     `json:"time_ms"`
		EnergyJ      float64     `json:"energy_j"`
		BatchTimeMS  float64     `json:"batch_time_ms"`
		SourceEnergy string      `json:"source_energy"`
		Placements   []pinRecord `json:"placements"`
	}{args[0], n, workers, batch, count, seconds * 1000 / float64(batch), energy / float64(batch), seconds * 1000, source, records})
}
