// exclusive_named_lock.go — named exclusive sections (Go)
//
// Same protocol as perf/exclusive_named_lock.ccs, rust/, and zig/.
// See the CC file header for the contract.
//
// Idiom: sync.Map of *sync.Mutex, create-on-first-use via LoadOrStore.
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
			ms := make([]*sync.Mutex, keys)
			for k := 0; k < keys; k++ {
				ms[k] = mutexFor(uint64(k))
			}
			r := rngForWorker(id)
			for i := 0; i < iters; i++ {
				k := zipfPick(zipfCum[:keys], keys, &r)
				ms[k].Lock()
				rowBump(&rows[k], spins)
				ms[k].Unlock()
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

func runUncontended(workers, iters, spins int) float64 {
	for i := 0; i < workers; i++ {
		rows[i].value = 0
	}

	t0 := time.Now()
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		id := w
		go func() {
			defer wg.Done()
			m := mutexFor(uint64(1000 + id))
			for i := 0; i < iters; i++ {
				m.Lock()
				rowBump(&rows[id], spins)
				m.Unlock()
			}
		}()
	}
	wg.Wait()
	ms := float64(time.Since(t0).Microseconds()) / 1000.0

	var sum int64
	for w := 0; w < workers; w++ {
		sum += rows[w].value
	}
	expect := int64(workers * iters)
	if sum != expect {
		fmt.Fprintf(os.Stderr, "UNCONTENDED CHECK FAIL: got=%d expect=%d\n", sum, expect)
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
	fmt.Printf("  note=sync.Mutex per name via sync.Map; Zipf uses CS work; "+
		"uncontended is lock micro (spins=0); no start-gun; goroutines GOMAXPROCS=%d; "+
		"time includes spawn+join\n", workers)

	warm := iters
	if warm > 1000 {
		warm = 1000
	}
	_ = runZipf(workers, warm, spins, keys)
	_ = runUncontended(workers, warm, 0)

	zSamples := make([]float64, trials)
	uSamples := make([]float64, trials)
	for t := 0; t < trials; t++ {
		zSamples[t] = runZipf(workers, iters, spins, keys)
		uSamples[t] = runUncontended(workers, iters, 0)
		fmt.Printf("  trial %d: zipf_ms=%.3f uncontended_ms=%.3f\n",
			t+1, zSamples[t], uSamples[t])
	}

	zMed := median(zSamples)
	uMed := median(uSamples)
	totalOps := float64(workers * iters)
	zOps := 0.0
	uOps := 0.0
	if zMed > 0 {
		zOps = totalOps / (zMed / 1000.0)
	}
	if uMed > 0 {
		uOps = totalOps / (uMed / 1000.0)
	}
	fmt.Printf("RESULT lang=go zipf_median_ms=%.3f zipf_ops_s=%.0f "+
		"uncontended_median_ms=%.3f uncontended_ops_s=%.0f\n",
		zMed, zOps, uMed, uOps)
}
