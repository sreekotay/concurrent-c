// exclusive_named_lock.go — named exclusive sections (Go)
//
// Same protocol as perf/exclusive_named_lock.ccs, rust/, and zig/.
// See the CC file header for the contract.
//
// Idiom: sync.Map of *sync.Mutex, create-on-first-use via LoadOrStore.
// Zipf locks by name each iteration (directory lookup on the hot path).
// Timing: before spawn through Wait (no start-gun).
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strconv"
	"sync"
	"time"
)

const (
	defaultWorkers = 8
	defaultIters   = 200000
	defaultTrials  = 7
	defaultWork    = 32
	defaultKeys    = 64
	defaultZipfS   = 1.0
	maxWorkers     = 256
	maxKeys        = 4096
	cacheLine      = 64
)

type row struct {
	value int64
	_     [cacheLine - 8]byte
}

type rng struct {
	state uint64
}

var (
	locks   sync.Map // uint64 -> *sync.Mutex
	rows    [maxKeys]row
	zipfCum [maxKeys]float64
)

func envInt(name string, fallback, min int) int {
	v := os.Getenv(name)
	if v == "" {
		return fallback
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < min {
		return fallback
	}
	return n
}

func envFloat(name string, fallback float64) float64 {
	v := os.Getenv(name)
	if v == "" {
		return fallback
	}
	n, err := strconv.ParseFloat(v, 64)
	if err != nil || n <= 0 {
		return fallback
	}
	return n
}

func mutexFor(name uint64) *sync.Mutex {
	if v, ok := locks.Load(name); ok {
		return v.(*sync.Mutex)
	}
	m := &sync.Mutex{}
	actual, _ := locks.LoadOrStore(name, m)
	return actual.(*sync.Mutex)
}

func csWork(spins int) {
	x := 0
	for i := 0; i < spins; i++ {
		x += i
	}
	runtime.KeepAlive(x)
}

func rowBump(r *row, spins int) {
	v := r.value
	csWork(spins)
	r.value = v + 1
}

func (r *rng) next() uint64 {
	x := r.state
	x ^= x << 13
	x ^= x >> 7
	x ^= x << 17
	r.state = x
	return x
}

func (r *rng) unit() float64 {
	return float64(r.next()>>11) * (1.0 / 9007199254740992.0)
}

func rngForWorker(workerID int) rng {
	state := uint64(0x9E3779B97F4A7C15) ^ (uint64(workerID) * 0xD1B54A32D192ED03)
	if state == 0 {
		state = 1
	}
	return rng{state: state}
}

func zipfInit(cum []float64, keys int, s float64) {
	total := 0.0
	for k := 0; k < keys; k++ {
		w := 1.0 / math.Pow(float64(k+1), s)
		total += w
		cum[k] = total
	}
	for k := 0; k < keys; k++ {
		cum[k] /= total
	}
}

func zipfPick(cum []float64, keys int, r *rng) int {
	u := r.unit()
	lo, hi := 0, keys-1
	for lo < hi {
		mid := (lo + hi) / 2
		if cum[mid] < u {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo
}

func runZipf(workers, iters, spins, keys int) float64 {
	for i := 0; i < keys; i++ {
		rows[i].value = 0
	}

	t0 := time.Now()
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		id := w
		go func() {
			defer wg.Done()
			r := rngForWorker(id)
			for i := 0; i < iters; i++ {
				k := zipfPick(zipfCum[:keys], keys, &r)
				m := mutexFor(uint64(k))
				m.Lock()
				rowBump(&rows[k], spins)
				m.Unlock()
			}
		}()
	}
	wg.Wait()
	ms := float64(time.Since(t0).Microseconds()) / 1000.0

	var sum int64
	for k := 0; k < keys; k++ {
		sum += rows[k].value
	}
	expect := int64(workers * iters)
	if sum != expect {
		fmt.Fprintf(os.Stderr, "ZIPF CHECK FAIL: got=%d expect=%d\n", sum, expect)
		os.Exit(1)
	}
	return ms
}

func runSerialFastpath(iters int) float64 {
	rows[0].value = 0
	m := mutexFor(1000)
	t0 := time.Now()
	for i := 0; i < iters; i++ {
		m.Lock()
		rowBump(&rows[0], 0)
		m.Unlock()
	}
	ms := float64(time.Since(t0).Microseconds()) / 1000.0

	if rows[0].value != int64(iters) {
		fmt.Fprintf(os.Stderr, "FASTPATH CHECK FAIL: got=%d expect=%d\n", rows[0].value, iters)
		os.Exit(1)
	}
	return ms
}

func median(samples []float64) float64 {
	s := append([]float64(nil), samples...)
	for i := 1; i < len(s); i++ {
		key := s[i]
		j := i - 1
		for j >= 0 && s[j] > key {
			s[j+1] = s[j]
			j--
		}
		s[j+1] = key
	}
	return s[len(s)/2]
}

func main() {
	workers := envInt("CC_EXCL_WORKERS", defaultWorkers, 1)
	iters := envInt("CC_EXCL_ITERS", defaultIters, 1)
	trials := envInt("CC_EXCL_TRIALS", defaultTrials, 1)
	spins := envInt("CC_EXCL_WORK", defaultWork, 0)
	keys := envInt("CC_EXCL_KEYS", defaultKeys, 1)
	zipfS := envFloat("CC_EXCL_ZIPF_S", defaultZipfS)
	if workers > maxWorkers {
		workers = maxWorkers
	}
	if keys > maxKeys {
		keys = maxKeys
	}
	zipfInit(zipfCum[:keys], keys, zipfS)

	runtime.GOMAXPROCS(workers)

	fmt.Printf("exclusive_named_lock lang=go\n")
	fmt.Printf("  workers=%d iters/worker=%d timed_trials=%d warmup=1 "+
		"cs_spins=%d keys=%d zipf_s=%.3f\n",
		workers, iters, trials, spins, keys, zipfS)
	fmt.Printf("  total_lock_ops/trial=%d\n", workers*iters)
	fmt.Printf("  note=zipf locks BY NAME each iter (directory+lock+scheduler "+
		"product, spawn+join, no start-gun, GOMAXPROCS=%d); serial_fastpath is "+
		"one resolved mutex, one caller, spins=0, no scheduler\n", workers)

	warm := iters
	if warm > 1000 {
		warm = 1000
	}
	_ = runZipf(workers, warm, spins, keys)
	_ = runSerialFastpath(workers * warm)

	zSamples := make([]float64, trials)
	fSamples := make([]float64, trials)
	totalOps := workers * iters
	for t := 0; t < trials; t++ {
		zSamples[t] = runZipf(workers, iters, spins, keys)
		fSamples[t] = runSerialFastpath(totalOps)
		fmt.Printf("  trial %d: zipf_ms=%.3f serial_fastpath_ms=%.3f\n",
			t+1, zSamples[t], fSamples[t])
	}

	zMed := median(zSamples)
	fMed := median(fSamples)
	ops := float64(totalOps)
	zOps := 0.0
	fOps := 0.0
	if zMed > 0 {
		zOps = ops / (zMed / 1000.0)
	}
	if fMed > 0 {
		fOps = ops / (fMed / 1000.0)
	}
	fmt.Printf("RESULT lang=go zipf_median_ms=%.3f zipf_ops_s=%.0f "+
		"serial_fastpath_median_ms=%.3f serial_fastpath_ops_s=%.0f\n",
		zMed, zOps, fMed, fOps)
}
