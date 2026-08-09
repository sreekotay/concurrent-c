// Go peer for stress/compare_arena_memory.sh — fair bump arena protocol.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
	"unsafe"
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

type slab struct {
	base []byte
	off  int
	prev *slab
}

type bump struct {
	cur       *slab
	nslabs    int
	blockMax  int
	ovf       [][]byte
	ovfBytes  int
	live      int
}

func (b *bump) init(root int, blockMax int) {
	b.cur = &slab{base: make([]byte, root)}
	b.nslabs = 1
	b.blockMax = blockMax
	b.ovf = nil
	b.ovfBytes = 0
	b.live = 0
}

func (b *bump) reset() {
	b.ovf = nil
	b.ovfBytes = 0
	for b.cur != nil && b.cur.prev != nil {
		b.cur = b.cur.prev
		b.nslabs--
	}
	if b.cur != nil {
		b.cur.off = 0
	}
	b.live = 0
	runtime.GC()
}

func (b *bump) free() {
	b.reset()
	b.cur = nil
	b.nslabs = 0
}

func alignUp(v, a int) int { return (v + a - 1) & ^(a - 1) }

func (b *bump) grow(need int) bool {
	if b.blockMax > 0 && b.nslabs >= b.blockMax {
		return false
	}
	cap := len(b.cur.base) + len(b.cur.base)/2
	if cap < need {
		cap = need
	}
	if cap < 4096 {
		cap = 4096
	}
	ns := &slab{base: make([]byte, cap), prev: b.cur}
	b.cur = ns
	b.nslabs++
	return true
}

func (b *bump) alloc(n, align int) []byte {
	off := alignUp(b.cur.off, align)
	end := off + n
	if end > len(b.cur.base) {
		if b.grow(n + align) {
			off = alignUp(b.cur.off, align)
			end = off + n
		} else {
			p := make([]byte, n)
			b.ovf = append(b.ovf, p)
			b.ovfBytes += n
			b.live += n
			return p
		}
	}
	p := b.cur.base[off:end:end]
	b.cur.off = end
	b.live += n
	return p
}

func (b *bump) realloc(ptr []byte, oldN, newN, align int, moves *uint64) []byte {
	if ptr == nil {
		return b.alloc(newN, align)
	}
	base := b.cur.base
	// Tip check: ptr aliases end of current slab.
	if len(base) > 0 && len(ptr) > 0 {
		start := int(uintptr(unsafe.Pointer(&ptr[0])) - uintptr(unsafe.Pointer(&base[0])))
		if start >= 0 && start < len(base) && start+oldN == b.cur.off {
			end := start + newN
			if newN <= oldN || end <= len(base) {
				b.cur.off = end
				b.live = b.live - oldN + newN
				return base[start:end:end]
			}
		}
	}
	np := b.alloc(newN, align)
	copy(np, ptr[:oldN])
	*moves++
	return np
}

func (b *bump) gross() int {
	g := b.ovfBytes
	for s := b.cur; s != nil; s = s.prev {
		g += len(s.base)
	}
	return g
}

func main() {
	tipIters := int(envZU("ARENA_MEM_TIP_ITERS", 20000))
	tipRoot := int(envZU("ARENA_MEM_TIP_ROOT", 4*1024*1024))
	stormN := int(envZU("ARENA_MEM_STORM", 50000))
	stormRoot := int(envZU("ARENA_MEM_ROOT_STORM", 4096))
	churnN := int(envZU("ARENA_MEM_CHURN", 200))
	churnEach := int(envZU("ARENA_MEM_CHURN_EACH", 64))
	blockMax := int(envZU("ARENA_MEM_BLOCK_MAX", 4))

	fmt.Printf("arena_memory_bench(go): block_max=%d tip_iters=%d storm=%d\n",
		blockMax, tipIters, stormN)

	var tip bump
	tip.init(tipRoot, blockMax)
	var moves uint64
	var p []byte
	sz := 0
	t0 := time.Now()
	for i := 0; i < tipIters; i++ {
		want := sz + 32 + (i % 17)
		p = tip.realloc(p, sz, want, 8, &moves)
		sz = want
	}
	tipMs := float64(time.Since(t0).Microseconds()) / 1000.0
	tip.free()

	var ms0, ms1 runtime.MemStats
	runtime.ReadMemStats(&ms0)
	var storm bump
	storm.init(stormRoot, blockMax)
	t0 = time.Now()
	for i := 0; i < stormN; i++ {
		n := 24 + (i*17)%512
		q := storm.alloc(n, 8)
		if len(q) > 0 {
			q[0] = byte(i)
		}
	}
	stormMs := float64(time.Since(t0).Microseconds()) / 1000.0
	stormGross := storm.gross()
	stormOvf := storm.ovfBytes
	storm.reset()
	resetGross := storm.gross()
	storm.free()
	runtime.ReadMemStats(&ms1)

	t0 = time.Now()
	for i := 0; i < churnN; i++ {
		var c bump
		c.init(8*1024, blockMax)
		for j := 0; j < churnEach; j++ {
			n := 16 + (j*31)%256
			_ = c.alloc(n, 8)
		}
		c.free()
	}
	churnMs := float64(time.Since(t0).Microseconds()) / 1000.0

	fmt.Printf("RESULT lang=go tip_ms=%.3f tip_moves=%d storm_ms=%.3f storm_gross=%d "+
		"storm_ovf=%d reset_gross=%d churn_ms=%.3f rss_delta=%d\n",
		tipMs, moves, stormMs, stormGross, stormOvf, resetGross, churnMs,
		int64(ms1.Sys)-int64(ms0.Sys))
}
