// Parallel handoff twin of perf/perf_parallel_handoff.ccs.
//
// Same N, one store per item. Go has no wait-for / @stage; the third
// row is an ordered write gate (mutex+cond, publish in i order).
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const defaultN = 100000

func envInt(key string, fallback int) int {
	s := os.Getenv(key)
	if s == "" {
		return fallback
	}
	v, err := strconv.Atoi(s)
	if err != nil || v <= 0 {
		return fallback
	}
	return v
}

func report(name string, elapsedMs float64, n int) {
	ops := 0.0
	ns := 0.0
	if elapsedMs > 0 {
		ops = float64(n) / (elapsedMs / 1000.0)
		ns = elapsedMs * 1.0e6 / float64(n)
	}
	fmt.Printf("  %s: %.0f ops/sec (%.3f ms, %.3f ns/item)\n", name, ops, elapsedMs, ns)
}

func verifyID(a []int) bool {
	for i, v := range a {
		if v != i {
			return false
		}
	}
	return true
}

func fillNeg(a []int) {
	for i := range a {
		a[i] = -1
	}
}

func seqLoop(buf []int) {
	for i := range buf {
		buf[i] = i
	}
}

// Idiomatic Go parallel-for: GOMAXPROCS contiguous chunks.
func parChunks(buf []int) {
	n := len(buf)
	p := runtime.GOMAXPROCS(0)
	chunk := (n + p - 1) / p
	var wg sync.WaitGroup
	for w := 0; w < p; w++ {
		lo := w * chunk
		hi := lo + chunk
		if hi > n {
			hi = n
		}
		if lo >= n {
			break
		}
		wg.Add(1)
		go func(lo, hi int) {
			defer wg.Done()
			for i := lo; i < hi; i++ {
				buf[i] = i
			}
		}(lo, hi)
	}
	wg.Wait()
}

// Ordered write gate: workers claim i, then publish only when pos==i.
func waitStage(buf []int) {
	n := len(buf)
	p := runtime.GOMAXPROCS(0)
	var job int64
	var mu sync.Mutex
	cond := sync.NewCond(&mu)
	pos := 0
	var wg sync.WaitGroup
	for w := 0; w < p; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				i := int(atomic.AddInt64(&job, 1) - 1)
				if i >= n {
					return
				}
				v := i
				mu.Lock()
				for pos != i {
					cond.Wait()
				}
				buf[pos] = v
				pos++
				cond.Broadcast()
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
}

func main() {
	n := envInt("CC_HANDOFF_N", defaultN)
	runtime.GOMAXPROCS(runtime.NumCPU())
	buf := make([]int, n)

	fmt.Println("parallel_handoff_go: seq / par-chunks / wait-stage (1 store per item)")

	t0 := time.Now()
	seqLoop(buf)
	elapsed := float64(time.Since(t0).Nanoseconds()) / 1e6
	if !verifyID(buf) {
		fmt.Fprintln(os.Stderr, "parallel_handoff_go: seq verify failed")
		os.Exit(1)
	}
	report("seq loop", elapsed, n)

	fillNeg(buf)
	t0 = time.Now()
	parChunks(buf)
	elapsed = float64(time.Since(t0).Nanoseconds()) / 1e6
	if !verifyID(buf) {
		fmt.Fprintln(os.Stderr, "parallel_handoff_go: par chunks verify failed")
		os.Exit(1)
	}
	report("par chunks", elapsed, n)

	fillNeg(buf)
	t0 = time.Now()
	waitStage(buf)
	elapsed = float64(time.Since(t0).Nanoseconds()) / 1e6
	if !verifyID(buf) {
		fmt.Fprintln(os.Stderr, "parallel_handoff_go: wait stage verify failed")
		os.Exit(1)
	}
	report("wait stage", elapsed, n)

	fmt.Println("parallel_handoff_go: DONE")
}
