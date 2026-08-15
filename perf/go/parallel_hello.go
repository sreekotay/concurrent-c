// Go counterpart to perf/parallel_hello_lowered.ccs.
//
//   CC_PAR_DEPTH    default 20
//   CC_PAR_SAMPLES  default 5
//
// Same depth-N binary reduction tree:
//   left  = sum(d+1, 2i)
//   right = sum(d+1, 2i+1)
//   return u24(left + right)
//
// Variants mirror the CC hand-lowering:
//   seq  — pure recursion
//   par  — spawn both arms, WaitGroup join
//   fork — spawn one arm, run the other on the caller, WaitGroup join
package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	warmup  = 2
	u24Mask = 0xffffff
)

var (
	samples    = 5
	depth      uint32 = 20
	spawnUntil uint32 = 0
)

func envU32(key string, fallback uint32) uint32 {
	s := os.Getenv(key)
	if s == "" {
		return fallback
	}
	n, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return fallback
	}
	return uint32(n)
}

func envGrains(fallback []uint32) []uint32 {
	s := os.Getenv("CC_PAR_GRAINS")
	if s == "" {
		return fallback
	}
	parts := strings.FieldsFunc(s, func(r rune) bool {
		return r == ',' || r == ' '
	})
	out := make([]uint32, 0, len(parts))
	for _, p := range parts {
		n, err := strconv.ParseUint(p, 10, 32)
		if err != nil {
			fmt.Fprintf(os.Stderr, "bad CC_PAR_GRAINS value %q\n", p)
			os.Exit(2)
		}
		out = append(out, uint32(n))
	}
	if len(out) == 0 {
		return fallback
	}
	return out
}

func u24(x uint32) uint32 { return x & u24Mask }

var (
	gSpawns atomic.Uint64
	gJoins  atomic.Uint64
)

func sumSeq(d, i uint32) uint32 {
	if d >= depth {
		return i
	}
	left := sumSeq(d+1, u24(i*2))
	right := sumSeq(d+1, u24(i*2+1))
	return u24(left + right)
}

func sumPar(d, i uint32) uint32 {
	if d >= depth {
		return i
	}
	if d >= spawnUntil {
		return u24(sumPar(d+1, u24(i*2)) + sumPar(d+1, u24(i*2+1)))
	}

	var left, right uint32
	var wg sync.WaitGroup
	wg.Add(2)
	gSpawns.Add(2)

	go func() {
		left = sumPar(d+1, u24(i*2))
		wg.Done()
	}()
	go func() {
		right = sumPar(d+1, u24(i*2+1))
		wg.Done()
	}()
	wg.Wait()
	gJoins.Add(1)
	return u24(left + right)
}

func sumFork(d, i uint32) uint32 {
	if d >= depth {
		return i
	}
	if d >= spawnUntil {
		return u24(sumFork(d+1, u24(i*2)) + sumFork(d+1, u24(i*2+1)))
	}

	var right uint32
	var wg sync.WaitGroup
	wg.Add(1)
	gSpawns.Add(1)

	go func() {
		right = sumFork(d+1, u24(i*2+1))
		wg.Done()
	}()
	left := sumFork(d+1, u24(i*2))
	wg.Wait()
	gJoins.Add(1)
	return u24(left + right)
}

func runOnGoro(fn func(uint32, uint32) uint32) uint32 {
	var result uint32
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		result = fn(0, 0)
		wg.Done()
	}()
	wg.Wait()
	return result
}

func timeOnce(fn func(uint32, uint32) uint32, expect uint32) float64 {
	t0 := time.Now()
	r := fn(0, 0)
	ms := float64(time.Since(t0).Microseconds()) / 1000.0
	if r != expect {
		panic(fmt.Sprintf("mismatch: got=%d expect=%d", r, expect))
	}
	return ms
}

func timeOnceGoro(fn func(uint32, uint32) uint32, expect uint32) float64 {
	t0 := time.Now()
	r := runOnGoro(fn)
	ms := float64(time.Since(t0).Microseconds()) / 1000.0
	if r != expect {
		panic(fmt.Sprintf("mismatch: got=%d expect=%d", r, expect))
	}
	return ms
}

func bench(name string, fn func(uint32, uint32) uint32, expect uint32) {
	xs := make([]float64, samples)
	var sum float64
	var spawns, joins uint64
	for i := 0; i < samples; i++ {
		gSpawns.Store(0)
		gJoins.Store(0)
		xs[i] = timeOnce(fn, expect)
		sum += xs[i]
		if i == 0 {
			spawns = gSpawns.Load()
			joins = gJoins.Load()
		}
	}
	sort.Float64s(xs)
	med := xs[samples/2]
	fmt.Printf("  %-4s  min %.2f  med %.2f  mean %.2f  max %.2f  ms",
		name, xs[0], med, sum/float64(samples), xs[samples-1])
	if spawns > 0 {
		fmt.Printf("  spawns=%d joins=%d  %.0f ns/spawn",
			spawns, joins, med*1e6/float64(spawns))
	}
	fmt.Printf("   %v\n", xs)
}

func benchGoro(name string, fn func(uint32, uint32) uint32, expect uint32) {
	xs := make([]float64, samples)
	var sum float64
	var spawns, joins uint64
	for i := 0; i < samples; i++ {
		gSpawns.Store(0)
		gJoins.Store(0)
		xs[i] = timeOnceGoro(fn, expect)
		sum += xs[i]
		if i == 0 {
			spawns = gSpawns.Load()
			joins = gJoins.Load()
		}
	}
	sort.Float64s(xs)
	med := xs[samples/2]
	fmt.Printf("  %-6s  min %.2f  med %.2f  mean %.2f  max %.2f  ms",
		name, xs[0], med, sum/float64(samples), xs[samples-1])
	if spawns > 0 {
		fmt.Printf("  spawns=%d joins=%d  %.0f ns/spawn",
			spawns, joins, med*1e6/float64(spawns))
	}
	fmt.Printf("   %v\n", xs)
}

const calibN = 8190

func noop() {}

func calibSerial() float64 {
	t0 := time.Now()
	for i := 0; i < calibN; i++ {
		var wg sync.WaitGroup
		wg.Add(1)
		go func() {
			noop()
			wg.Done()
		}()
		wg.Wait()
	}
	return float64(time.Since(t0).Microseconds()) / 1000.0
}

func calibSerialFromGoro() float64 {
	t0 := time.Now()
	var outer sync.WaitGroup
	outer.Add(1)
	go func() {
		for i := 0; i < calibN; i++ {
			var wg sync.WaitGroup
			wg.Add(1)
			go func() {
				noop()
				wg.Done()
			}()
			wg.Wait()
		}
		outer.Done()
	}()
	outer.Wait()
	return float64(time.Since(t0).Microseconds()) / 1000.0
}

func calibBatch() float64 {
	t0 := time.Now()
	var wg sync.WaitGroup
	wg.Add(calibN)
	for i := 0; i < calibN; i++ {
		go func() {
			noop()
			wg.Done()
		}()
	}
	wg.Wait()
	return float64(time.Since(t0).Microseconds()) / 1000.0
}

func calibReport(name string, fn func() float64) {
	xs := make([]float64, samples)
	for i := 0; i < warmup; i++ {
		_ = fn()
	}
	for i := 0; i < samples; i++ {
		xs[i] = fn()
	}
	sort.Float64s(xs)
	med := xs[samples/2]
	fmt.Printf("  %-18s  med %.2f ms  %.0f ns/op  (N=%d)\n",
		name, med, med*1e6/float64(calibN), calibN)
}

func medianOf(fn func(uint32, uint32) uint32, expect uint32, onGoro bool) (float64, uint64) {
	xs := make([]float64, samples)
	var spawns uint64
	for i := 0; i < warmup; i++ {
		if onGoro {
			_ = timeOnceGoro(fn, expect)
		} else {
			_ = timeOnce(fn, expect)
		}
	}
	for i := 0; i < samples; i++ {
		gSpawns.Store(0)
		gJoins.Store(0)
		if onGoro {
			xs[i] = timeOnceGoro(fn, expect)
		} else {
			xs[i] = timeOnce(fn, expect)
		}
		if i == 0 {
			spawns = gSpawns.Load()
		}
	}
	sort.Float64s(xs)
	return xs[samples/2], spawns
}

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU())
	depth = envU32("CC_PAR_DEPTH", 20)
	if n := envU32("CC_PAR_SAMPLES", 5); n >= 1 {
		samples = int(n)
	}
	grains := envGrains([]uint32{0, 1, 2, 3, 4, 6, 8})

	fmt.Printf("parallel_hello_go grain sweep: depth=%d samples=%d GOMAXPROCS=%d (fork on goro)\n",
		depth, samples, runtime.GOMAXPROCS(0))
	fmt.Println("  spawn while d < grain; below that, sequential")

	spawnUntil = 0
	expect := sumSeq(0, 0)
	seqMs, _ := medianOf(sumSeq, expect, false)
	fmt.Printf("  seq  %d  %.2f ms\n", expect, seqMs)
	fmt.Println("  grain  spawns     fork_ms   vs_seq")

	for _, g := range grains {
		spawnUntil = g
		forkMs, spawns := medianOf(sumFork, expect, true)
		vs := 0.0
		if forkMs > 0 {
			vs = seqMs / forkMs
		}
		fmt.Printf("  %5d  %7d  %9.2f  %6.2fx\n", g, spawns, forkMs, vs)
	}
}
