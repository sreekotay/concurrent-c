// Idiomatic mixed-lifetime peer: allocate, drop half, grow survivors, GC reclaim.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
)

func envZU(k string, d uint64) uint64 {
	v := os.Getenv(k)
	if v == "" {
		return d
	}
	n, err := strconv.ParseUint(v, 10, 64)
	if err != nil {
		return d
	}
	return n
}

func itemSize(i int) int { return 24 + (i*17)%400 }

func lcg(s *uint32) uint32 {
	*s = (*s * 1664525) + 1013904223
	return *s
}

func main() {
	n := int(envZU("MIX_LIFE_N", 200000))
	seed := uint32(envZU("MIX_LIFE_SEED", 0xC0FFEE))
	ptrs := make([][]byte, n)
	alive := make([]bool, n)
	order := make([]int, n)
	rng := seed
	var sink byte

	fmt.Printf("mixed_lifetime(go): n=%d seed=%d\n", n, seed)

	var ms0, msPeak, ms1 runtime.MemStats
	runtime.ReadMemStats(&ms0)
	msPeak = ms0
	t0 := time.Now()

	t := time.Now()
	live := 0
	for i := 0; i < n; i++ {
		sz := itemSize(i)
		b := make([]byte, sz)
		b[0] = byte(i)
		sink ^= b[0]
		ptrs[i] = b
		alive[i] = true
		live++
	}
	allocMs := float64(time.Since(t).Microseconds()) / 1000.0
	peakLive := live
	runtime.ReadMemStats(&msPeak)

	for i := 0; i < n; i++ {
		order[i] = i
	}
	for i := n; i > 1; i-- {
		j := int(lcg(&rng) % uint32(i))
		order[i-1], order[j] = order[j], order[i-1]
	}

	t = time.Now()
	freed := 0
	for i := 0; i < n/2; i++ {
		idx := order[i]
		if !alive[idx] {
			continue
		}
		ptrs[idx] = nil
		alive[idx] = false
		live--
		freed++
	}
	runtime.GC()
	midfreeMs := float64(time.Since(t).Microseconds()) / 1000.0

	t = time.Now()
	for i := 0; i < n; i++ {
		if !alive[i] {
			continue
		}
		want := len(ptrs[i]) + 48 + (i % 64)
		nb := make([]byte, want)
		copy(nb, ptrs[i])
		nb[0] = byte(i ^ 0x5a)
		sink ^= nb[0]
		ptrs[i] = nb
	}
	growMs := float64(time.Since(t).Microseconds()) / 1000.0
	runtime.ReadMemStats(&ms1)
	if ms1.Sys > msPeak.Sys {
		msPeak = ms1
	}

	t = time.Now()
	for i := 0; i < n; i++ {
		ptrs[i] = nil
		alive[i] = false
	}
	runtime.GC()
	reclaimMs := float64(time.Since(t).Microseconds()) / 1000.0
	totalMs := float64(time.Since(t0).Microseconds()) / 1000.0
	runtime.ReadMemStats(&ms1)
	runtime.KeepAlive(sink)

	fmt.Printf("RESULT lang=go strategy=gc n=%d freed=%d peak_live=%d "+
		"alloc_ms=%.3f midfree_ms=%.3f grow_ms=%.3f reclaim_ms=%.3f total_ms=%.3f "+
		"rss_peak_delta=%d rss_end_delta=%d hwm=0 cgroup_peak=0 sink=%d\n",
		n, freed, peakLive, allocMs, midfreeMs, growMs, reclaimMs, totalMs,
		int64(msPeak.Sys)-int64(ms0.Sys), int64(ms1.Sys)-int64(ms0.Sys), sink)
}
