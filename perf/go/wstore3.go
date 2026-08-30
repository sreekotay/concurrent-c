// wstore3.go — death-time buckets, per shard (Go twin of CCEpochKeys).
//
// Same CLI and mix as perf/wstore3.ccs.
// put / putExpires / expire / persist / delete / get.
// TTL lands on the wheel; never is expires-none only. get does not collect.
// Records share one freelist per shard (drained-slot storage is reused).
// FREE=0: no freelist; expire drops the pointer; trim runs GC+FreeOSMemory.
// CC twin uses per-generation arenas (not this list).
//
//	go run ./perf/go/wstore3.go <sec> <sweep 0|1> [theta] [wave] [shards] [clients] [write|get|drain]
//	Compare: ./perf/compare_wstore3.sh stand
//	         FREE=0 ./perf/compare_wstore3.sh stand
//
// Does not pin GOMAXPROCS. Report what the runtime chose.
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

/* FREE=0: no per-shard reuse list. Default is the reuse list. */
var useFree = true

const (
	levels        = 3
	maxSlots      = 64
	neverMs       = int64(math.MaxInt64)
	maxShards     = 64
	maxClients    = 64
	waveHoldUs     = 100.0
	waveHoldSample = 16 /* clock every N steps; wall is the only cap */
	hitchUs       = 1000.0
	keyCap        = 32
	valCap        = 128
	neverPool     = 20000
	recentN       = 8192
	trimKeys      = 1_000_000 /* post-mix: expire to this many, then mem: trim */
)

var (
	kUnitMs = [levels]int64{100, 1000, 60000}
	kSlots  = [levels]int{64, 64, 60}
)

type entry struct {
	keyLen, valLen int
	keyBuf         [keyCap]byte
	valBuf         [valCap]byte
	diesAt         int64
	dead           uint8
	level          int8
	slot           uint8
	liveNext       *entry
	livePrev       *entry
	freeNext       *entry
}

func (e *entry) keyBytes() []byte { return e.keyBuf[:e.keyLen] }

type epoch struct {
	liveHead  *entry
	liveTail  *entry
	liveN     int
	liveBytes int
	deadBytes int
}

type bucket struct {
	cur      epoch
	scanE    *entry
	scanning bool
	end      int64
}

type wheelSlot struct {
	b     *bucket
	level int8
	slot  uint8
}

type shard struct {
	idx        int
	mu         sync.Mutex
	free       *entry /* one freelist; drained-slot storage is reused */
	never      bucket
	wheel      [levels][maxSlots]bucket
	m          map[string]*entry
	waveRecs   int
	sweepOn    bool
	stop       atomic.Bool
	sweepDue   atomic.Bool
	nextEnd    atomic.Int64
	endsDirty  bool
	kick       chan struct{}
	drains     int
	drainedRec int
	dropped    int
	reinsert   int
	rehashes   int
	waves      int
	waveMaxUs  float64
	waveSumUs  float64
	holdT0     float64
	holdSteps  int
	ops        int
	gets       int
	getHits    int
}

type store struct {
	nshards int
	shards  []*shard
}

type client struct {
	st       *store
	id       int
	nclients int
	mixGet   bool
	mixDrain bool
	rng      uint64
	tStart   float64
	tEnd     float64
	ops      int64
	recent   [recentN][32]byte
	recentN  int
}

func nowUs() float64 {
	return float64(time.Now().UnixNano()) / 1e3
}

func nowMs() int64 { return int64(nowUs() / 1000.0) }

func (s *shard) arm() { s.sweepDue.Store(true) }

func (s *shard) kickSweep() {
	s.arm()
	select {
	case s.kick <- struct{}{}:
	default:
	}
}

func (s *shard) refreshNextEnd() {
	m := neverMs
	for l := 0; l < levels; l++ {
		for i := 0; i < kSlots[l]; i++ {
			if s.wheel[l][i].end < m {
				m = s.wheel[l][i].end
			}
		}
	}
	s.nextEnd.Store(m)
	s.endsDirty = false
}

func (s *shard) noteSlotEnd(oldEnd, newEnd int64) {
	_ = newEnd
	if oldEnd == s.nextEnd.Load() {
		s.endsDirty = true
	}
}

func entryFill(e *entry, key, val []byte) bool {
	if len(key) > keyCap || len(val) > valCap {
		return false
	}
	e.keyLen = copy(e.keyBuf[:], key)
	e.valLen = copy(e.valBuf[:], val)
	return true
}

func (e *epoch) clearLive() {
	e.liveHead = nil
	e.liveTail = nil
	e.liveN = 0
	e.liveBytes = 0
	e.deadBytes = 0
}

func (e *epoch) link(x *entry) {
	x.liveNext = nil
	x.livePrev = e.liveTail
	if e.liveTail != nil {
		e.liveTail.liveNext = x
	} else {
		e.liveHead = x
	}
	e.liveTail = x
	e.liveN++
}

func (e *epoch) unlink(x *entry) {
	if x.livePrev != nil {
		x.livePrev.liveNext = x.liveNext
	} else if e.liveHead == x {
		e.liveHead = x.liveNext
	}
	if x.liveNext != nil {
		x.liveNext.livePrev = x.livePrev
	} else if e.liveTail == x {
		e.liveTail = x.livePrev
	}
	x.liveNext = nil
	x.livePrev = nil
	if e.liveN > 0 {
		e.liveN--
	}
}

func mapKey(b []byte) string {
	return unsafe.String(unsafe.SliceData(b), len(b))
}

func (s *shard) find(k []byte) *entry {
	return s.m[mapKey(k)]
}

func (s *shard) slotFor(now, diesAt int64) wheelSlot {
	for l := 0; l < levels; l++ {
		unit := kUnitMs[l]
		span := unit * int64(kSlots[l])
		if diesAt-now > span {
			continue
		}
		idx := int((diesAt / unit) % int64(kSlots[l]))
		b := &s.wheel[l][idx]
		if diesAt >= b.end-unit && diesAt < b.end {
			return wheelSlot{b: b, level: int8(l), slot: uint8(idx)}
		}
	}
	l := levels - 1
	unit := kUnitMs[l]
	idx := int((diesAt / unit) % int64(kSlots[l]))
	return wheelSlot{b: &s.wheel[l][idx], level: int8(l), slot: uint8(idx)}
}

func (s *shard) bucketOf(e *entry) *bucket {
	if e.level < 0 {
		return &s.never
	}
	return &s.wheel[e.level][e.slot]
}

func (s *shard) alloc() *entry {
	if useFree && s.free != nil {
		x := s.free
		s.free = x.freeNext
		*x = entry{}
		return x
	}
	return new(entry)
}

func (s *shard) freeOne(x *entry) {
	if !useFree {
		return
	}
	x.freeNext = s.free
	s.free = x
}

func (s *shard) markDead(e *entry) {
	if e == nil || e.dead != 0 {
		return
	}
	e.dead = 1
	b := s.bucketOf(e)
	if b == nil {
		s.freeOne(e)
		return
	}
	n := e.keyLen + e.valLen
	if b.scanE == e {
		b.scanE = e.liveNext
	}
	b.cur.unlink(e)
	if b.cur.liveBytes >= n {
		b.cur.liveBytes -= n
	} else {
		b.cur.liveBytes = 0
	}
	b.cur.deadBytes += n
	if s.m[mapKey(e.keyBytes())] == e {
		delete(s.m, mapKey(e.keyBytes()))
	}
	s.freeOne(e)
}

func (s *shard) place(sl wheelSlot, key, val []byte, diesAt int64) {
	old := s.find(key)
	b := sl.b
	if old != nil && old.dead == 0 && s.bucketOf(old) == b && len(val) <= valCap {
		oldN := old.valLen
		old.valLen = copy(old.valBuf[:], val)
		if oldN > old.valLen {
			b.cur.liveBytes -= oldN - old.valLen
		} else {
			b.cur.liveBytes += old.valLen - oldN
		}
		old.diesAt = diesAt
		return
	}
	e := s.alloc()
	e.dead = 1
	if !entryFill(e, key, val) {
		s.freeOne(e)
		panic("entry cap")
	}
	e.diesAt = diesAt
	e.level = sl.level
	e.slot = sl.slot
	/* Delete first. Assign of an equal string keeps the old key header,
	   which aliases old.keyBuf — then freeOne mutates it. */
	if old != nil {
		delete(s.m, mapKey(old.keyBytes()))
	}
	s.m[mapKey(e.keyBytes())] = e
	e.dead = 0
	b.cur.link(e)
	b.cur.liveBytes += e.keyLen + e.valLen
	if old != nil {
		s.markDead(old)
	}
}

func (s *shard) put(key, val []byte) {
	s.place(wheelSlot{b: &s.never, level: -1}, key, val, neverMs)
}

func (s *shard) putExpires(now int64, key, val []byte, diesAt int64) {
	if diesAt <= now {
		s.delete(key)
		return
	}
	s.place(s.slotFor(now, diesAt), key, val, diesAt)
}

func (s *shard) expire(now int64, key []byte, diesAt int64) {
	old := s.find(key)
	if old == nil {
		return
	}
	if diesAt <= now {
		s.delete(key)
		return
	}
	sl := s.slotFor(now, diesAt)
	if s.bucketOf(old) == sl.b {
		old.diesAt = diesAt
		return
	}
	s.place(sl, old.keyBytes(), old.valBuf[:old.valLen], diesAt)
}

func (s *shard) persist(key []byte) {
	old := s.find(key)
	if old == nil {
		return
	}
	if old.level < 0 {
		old.diesAt = neverMs
		return
	}
	s.place(wheelSlot{b: &s.never, level: -1}, old.keyBytes(), old.valBuf[:old.valLen], neverMs)
}

func (s *shard) delete(key []byte) {
	e := s.find(key)
	if e == nil {
		return
	}
	s.markDead(e)
}

func (s *shard) get(key []byte) *entry {
	s.gets++
	e := s.find(key)
	if e != nil {
		s.getHits++
	}
	return e
}

func (s *shard) holdUp() bool {
	s.holdSteps++
	if s.holdSteps&(waveHoldSample-1) != 0 {
		return false
	}
	return (nowUs() - s.holdT0) >= waveHoldUs
}

func (s *shard) rotate(l, i int) {
	b := &s.wheel[l][i]
	old := b.end
	b.end += kUnitMs[l] * int64(kSlots[l])
	b.scanning = false
	b.scanE = nil
	s.noteSlotEnd(old, b.end)
}

func (s *shard) scanDue(b *bucket, level int8, slot uint8, now int64, budget int) int {
	n := 0
	e := b.scanE
	if e == nil {
		e = b.cur.liveHead
	}
	for e != nil && n < budget && !s.holdUp() {
		nxt := e.liveNext
		n++
		s.drainedRec++
		b.scanE = nxt
		if e.diesAt <= now {
			s.markDead(e)
			s.dropped++
		}
		e = nxt
	}
	if e == nil {
		b.scanE = nil
		if b.cur.liveN == 0 {
			b.cur.clearLive()
		}
		s.rotate(int(level), int(slot))
		s.drains++
	}
	return n
}

func (s *shard) retire(l, i int) {
	b := &s.wheel[l][i]
	if b.scanning {
		return
	}
	if b.cur.liveN == 0 {
		b.cur.clearLive()
		s.rotate(l, i)
		s.drains++
		return
	}
	b.scanning = true
	s.arm()
}

func (s *shard) wheelScanning() bool {
	for l := 0; l < levels; l++ {
		for i := 0; i < kSlots[l]; i++ {
			if s.wheel[l][i].scanning {
				return true
			}
		}
	}
	return false
}

func (s *shard) more(now int64) bool {
	if now >= s.nextEnd.Load() {
		return true
	}
	return s.wheelScanning()
}

func (s *shard) prepare(_ int64) {}

func (s *shard) noteHold(verb string, us float64, n int) {
	s.waves++
	s.waveSumUs += us
	if us > s.waveMaxUs {
		s.waveMaxUs = us
	}
	if us >= hitchUs {
		fmt.Printf("# hitch shard=%d verb=%s us=%.0f n=%d\n", s.idx, verb, us, n)
	}
}

func (s *shard) sweepWave() {
	s.prepare(nowMs())
	s.mu.Lock()
	s.holdT0 = nowUs()
	s.holdSteps = 0
	now := nowMs()
	s.sweepDue.Store(false)
	n := 0
	if now >= s.nextEnd.Load() {
		for l := 0; l < levels; l++ {
			for i := 0; i < kSlots[l]; i++ {
				if s.wheel[l][i].end > now {
					continue
				}
				s.retire(l, i)
				n++
			}
		}
	}
	if !s.holdUp() {
		scanned := 0
		for l := 0; l < levels && scanned < s.waveRecs && !s.holdUp(); l++ {
			for i := 0; i < kSlots[l] && scanned < s.waveRecs && !s.holdUp(); i++ {
				b := &s.wheel[l][i]
				if !b.scanning {
					continue
				}
				scanned += s.scanDue(b, int8(l), uint8(i), now, s.waveRecs-scanned)
			}
		}
		n += scanned
	}
	if s.endsDirty {
		s.refreshNextEnd()
	}
	us := nowUs() - s.holdT0
	if s.more(nowMs()) {
		s.arm()
	}
	s.mu.Unlock()
	if n > 0 {
		s.noteHold("expire", us, n)
	}
}

func (s *shard) sweeper() {
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	if !timer.Stop() {
		<-timer.C
	}
	for !s.stop.Load() {
		if !s.sweepDue.Load() && nowMs() < s.nextEnd.Load() {
			wait := s.nextEnd.Load() - nowMs()
			if wait < 1 {
				wait = 1
			}
			timer.Reset(time.Duration(wait) * time.Millisecond)
			select {
			case <-timer.C:
			case <-s.kick:
				if !timer.Stop() {
					select {
					case <-timer.C:
					default:
					}
				}
			}
		}
		if s.stop.Load() {
			return
		}
		s.sweepWave()
		runtime.Gosched()
	}
}

func (s *shard) init(idx int, wave int, sweepOn bool) {
	s.idx = idx
	s.m = make(map[string]*entry, 4096)
	s.waveRecs = wave
	s.sweepOn = sweepOn
	s.kick = make(chan struct{}, 1)
	now := nowMs()
	s.never.end = neverMs
	for l := 0; l < levels; l++ {
		unit := kUnitMs[l]
		for i := 0; i < kSlots[l]; i++ {
			base := (now / unit) * unit
			end := base + unit
			for int((end/unit)%int64(kSlots[l])) != (i+1)%kSlots[l] {
				end += unit
			}
			s.wheel[l][i] = bucket{end: end}
		}
	}
	s.refreshNextEnd()
}

func (s *shard) keys() int { return len(s.m) }

func (s *shard) liveEntries() int {
	t := s.never.cur.liveN
	for l := 0; l < levels; l++ {
		for i := 0; i < kSlots[l]; i++ {
			t += s.wheel[l][i].cur.liveN
		}
	}
	return t
}

func (s *shard) logical() int {
	t := s.never.cur.liveBytes
	for l := 0; l < levels; l++ {
		for i := 0; i < kSlots[l]; i++ {
			t += s.wheel[l][i].cur.liveBytes
		}
	}
	return t
}

func keyHash(k []byte) uint64 {
	h := uint64(0xcbf29ce484222325)
	for _, c := range k {
		h ^= uint64(c)
		h *= 0x100000001b3
	}
	return h
}

func (st *store) shardOf(k []byte) *shard {
	return st.shards[keyHash(k)%uint64(st.nshards)]
}

func (st *store) epochsLive() int {
	n := 0
	for _, s := range st.shards {
		if s.never.cur.liveN > 0 {
			n++
		}
		for l := 0; l < levels; l++ {
			for i := 0; i < kSlots[l]; i++ {
				if s.wheel[l][i].cur.liveN > 0 {
					n++
				}
			}
		}
	}
	return n
}

func (st *store) liveEntries() int {
	n := 0
	for _, s := range st.shards {
		n += s.liveEntries()
	}
	return n
}

func (st *store) mapKeys() int {
	n := 0
	for _, s := range st.shards {
		n += s.keys()
	}
	return n
}

func (s *shard) epochKill(ep *epoch, want int) int {
	n := 0
	e := ep.liveHead
	for e != nil && n < want {
		nxt := e.liveNext
		s.markDead(e)
		n++
		e = nxt
	}
	return n
}

func (s *shard) trim(want int) int {
	n := 0
	for l := 0; l < levels && n < want; l++ {
		for i := 0; i < kSlots[l] && n < want; i++ {
			n += s.epochKill(&s.wheel[l][i].cur, want-n)
		}
	}
	if n < want {
		n += s.epochKill(&s.never.cur, want-n)
	}
	return n
}

func (st *store) trimTo(target int) {
	live := st.liveEntries()
	if live <= target {
		return
	}
	need := live - target
	for _, s := range st.shards {
		if need <= 0 {
			return
		}
		s.mu.Lock()
		have := s.liveEntries()
		take := need
		if take > have {
			take = have
		}
		need -= s.trim(take)
		s.mu.Unlock()
	}
}

func printMem(st *store, when string) {
	fmt.Printf("# mem: %s commit_KiB=%d rss_KiB=%d epochs=%d live_n=%d map_n=%d\n",
		when, heapKiB(), rssKiB(), st.epochsLive(), st.liveEntries(), st.mapKeys())
}

func rssKiB() uint64 {
	if runtime.GOOS == "linux" {
		f, err := os.Open("/proc/self/status")
		if err == nil {
			var buf [4096]byte
			n, _ := f.Read(buf[:])
			_ = f.Close()
			for _, line := range splitLines(buf[:n]) {
				if len(line) > 6 && string(line[:6]) == "VmRSS:" {
					var v uint64
					fmt.Sscanf(string(line[6:]), "%d", &v)
					return v
				}
			}
		}
	}
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	return m.Sys / 1024
}

func heapKiB() uint64 {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	return m.HeapSys / 1024
}

func splitLines(b []byte) [][]byte {
	var out [][]byte
	start := 0
	for i, c := range b {
		if c == '\n' {
			out = append(out, b[start:i])
			start = i + 1
		}
	}
	if start < len(b) {
		out = append(out, b[start:])
	}
	return out
}

func (c *client) rnd() uint64 {
	c.rng ^= c.rng << 13
	c.rng ^= c.rng >> 7
	c.rng ^= c.rng << 17
	return c.rng
}

func (c *client) rndf() float64 {
	return float64(c.rnd()>>11) / 9007199254740992.0
}

func (c *client) run() {
	var key [64]byte
	val := make([]byte, 96)
	for i := range val {
		val[i] = 'v'
	}
	seq := int64(0)
	pTTL, pNever, pDel := 0.55, 0.70, 0.85
	if c.mixGet {
		pTTL, pNever, pDel = 0.20, 0.25, 0.30
	}
	for i := int64(0); ; i++ {
		t := nowUs()
		if t >= c.tEnd {
			return
		}
		if c.mixDrain && t >= c.tStart+(c.tEnd-c.tStart)*0.10 {
			runtime.Gosched()
			continue
		}
		now := int64(t / 1000.0)
		r := c.rndf()
		n := 0
		if c.mixDrain {
			if c.nclients == 1 {
				n = putKey(key[:], 'e', -1, seq)
			} else {
				n = putKey(key[:], 'e', c.id, seq)
			}
			seq++
			kb := key[:n]
			s := c.st.shardOf(kb)
			s.mu.Lock()
			s.ops++
			s.putExpires(now, kb, val, now+200)
			s.mu.Unlock()
			c.ops++
			if i&255 == 0 {
				runtime.Gosched()
			}
			continue
		}
		if r < pTTL {
			if c.nclients == 1 {
				n = putKey(key[:], 'e', -1, seq)
			} else {
				n = putKey(key[:], 'e', c.id, seq)
			}
			seq++
		} else if r < pNever {
			id := int64(float64(neverPool) * math.Pow(c.rndf(), 3.0))
			n = putKey(key[:], 'n', -1, id)
		} else if r < pDel {
			if c.rnd()&1 != 0 && c.recentN > 0 {
				lim := c.recentN
				if lim > recentN {
					lim = recentN
				}
				src := c.recent[c.rnd()%uint64(lim)]
				n = copy(key[:], bytesTrim(src[:]))
			} else {
				id := int64(float64(neverPool) * math.Pow(c.rndf(), 3.0))
				n = putKey(key[:], 'n', -1, id)
			}
		} else if c.mixGet && c.recentN > 0 && c.rnd()&3 == 0 {
			lim := c.recentN
			if lim > recentN {
				lim = recentN
			}
			src := c.recent[c.rnd()%uint64(lim)]
			n = copy(key[:], bytesTrim(src[:]))
		} else {
			id := int64(float64(neverPool) * c.rndf())
			n = putKey(key[:], 'n', -1, id)
		}
		kb := key[:n]
		s := c.st.shardOf(kb)
		s.mu.Lock()
		s.ops++
		if r < pTTL {
			ttl := 50.0 * math.Pow(400.0, c.rndf())
			s.putExpires(now, kb, val, now+int64(ttl))
			copy(c.recent[c.recentN%recentN][:], kb)
			c.recentN++
		} else if r < pNever {
			s.put(kb, val)
		} else if r < pDel {
			s.delete(kb)
		} else {
			_ = s.get(kb)
		}
		s.mu.Unlock()
		c.ops++
		if i&255 == 0 {
			runtime.Gosched()
		}
	}
}

func (st *store) preloadNever() {
	val := make([]byte, 96)
	for i := range val {
		val[i] = 'v'
	}
	var key [64]byte
	for id := int64(0); id < neverPool; id++ {
		n := putKey(key[:], 'n', -1, id)
		kb := key[:n]
		s := st.shardOf(kb)
		s.mu.Lock()
		s.put(kb, val)
		s.mu.Unlock()
	}
}

func putKey(buf []byte, kind byte, id int, seq int64) int {
	b := buf[:0]
	b = append(b, kind, ':')
	if id >= 0 {
		b = strconv.AppendInt(b, int64(id), 10)
		b = append(b, ':')
	}
	b = strconv.AppendInt(b, seq, 10)
	return len(b)
}

func bytesTrim(b []byte) []byte {
	for i, c := range b {
		if c == 0 {
			return b[:i]
		}
	}
	return b
}

func reporter(st *store, t0, tEnd float64) {
	fmt.Printf("# %6s %9s %11s %12s %9s %6s %8s %8s %8s %6s %8s\n",
		"t_s", "keys", "logical_KiB", "commit_KiB", "rss_KiB",
		"drains", "dropped", "reinsert", "waves", "rehash", "ops")
	for {
		time.Sleep(time.Second)
		if nowUs() >= tEnd {
			return
		}
		keys, logical, drains, dropped, reins, waves, rehash, ops := 0, 0, 0, 0, 0, 0, 0, 0
		for _, s := range st.shards {
			s.mu.Lock()
			keys += s.keys()
			logical += s.logical()
			drains += s.drains
			dropped += s.dropped
			reins += s.reinsert
			waves += s.waves
			rehash += s.rehashes
			ops += s.ops
			s.mu.Unlock()
		}
		fmt.Printf("  %6.0f %9d %11d %12d %9d %6d %8d %8d %8d %6d %8d\n",
			(nowUs()-t0)/1e6, keys, logical/1024, heapKiB(), rssKiB(),
			drains, dropped, reins, waves, rehash, ops)
	}
}

func argInt(i, def int) int {
	if len(os.Args) <= i {
		return def
	}
	n, err := strconv.Atoi(os.Args[i])
	if err != nil {
		return def
	}
	return n
}

func argFloat(i int, def float64) float64 {
	if len(os.Args) <= i {
		return def
	}
	f, err := strconv.ParseFloat(os.Args[i], 64)
	if err != nil {
		return def
	}
	return f
}

func envFree() bool {
	switch os.Getenv("FREE") {
	case "0", "off", "no":
		return false
	default:
		return true
	}
}

func main() {
	useFree = envFree()
	seconds := argFloat(1, 20)
	sweep := argInt(2, 1)
	theta := argFloat(3, 0.35)
	wave := argInt(4, 512)
	nshards := argInt(5, 1)
	nclients := argInt(6, 1)
	mixGet := false
	mixDrain := false
	if len(os.Args) > 7 {
		switch os.Args[7] {
		case "get":
			mixGet = true
		case "drain":
			mixDrain = true
		case "write":
		default:
			fmt.Fprintf(os.Stderr, "mix must be write, get, or drain\n")
			os.Exit(1)
		}
	}
	if nshards < 1 {
		nshards = 1
	}
	if nshards > maxShards {
		nshards = maxShards
	}
	if nclients < 1 {
		nclients = 1
	}
	if nclients > maxClients {
		nclients = maxClients
	}

	st := &store{nshards: nshards, shards: make([]*shard, nshards)}
	for i := 0; i < nshards; i++ {
		s := &shard{}
		s.init(i, wave, sweep != 0)
		st.shards[i] = s
	}

	mixName := "write"
	if mixDrain {
		mixName = "drain"
	} else if mixGet {
		mixName = "get"
	}
	fmt.Printf("# wstore3-go seconds=%.0f sweep=%d mix=%s theta=%.2f wave_recs=%d "+
		"shards=%d clients=%d gomaxprocs=%d freelist=%d "+
		"wheel=100ms x64, 1s x64, 60s x60\n",
		seconds, sweep, mixName, theta, wave, nshards, nclients, runtime.GOMAXPROCS(0),
		map[bool]int{true: 1, false: 0}[useFree])
	printMem(st, "idle")
	if mixGet {
		st.preloadNever()
	}

	t0 := nowUs()
	tEnd := t0 + seconds*1e6
	clients := make([]*client, nclients)
	var wg sync.WaitGroup
	if sweep != 0 {
		for i := 0; i < nshards; i++ {
			s := st.shards[i]
			wg.Add(1)
			go func() {
				defer wg.Done()
				s.sweeper()
			}()
		}
	}
	for i := 0; i < nclients; i++ {
		c := &client{
			st:       st,
			id:       i,
			nclients: nclients,
			mixGet:   mixGet,
			mixDrain: mixDrain,
			rng:      0x9E3779B97F4A7C15,
			tStart:   t0,
			tEnd:     tEnd,
		}
		if i != 0 {
			c.rng ^= uint64(i+1) * 0xD1B54A32D192ED03
		}
		clients[i] = c
		wg.Add(1)
		go func() {
			defer wg.Done()
			c.run()
		}()
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		reporter(st, t0, tEnd)
		for _, s := range st.shards {
			s.stop.Store(true)
			s.kickSweep()
		}
	}()
	wg.Wait()

	var ops int64
	drains, drec, dropped, reins, rehash, waves := 0, 0, 0, 0, 0, 0
	gets, hits := 0, 0
	wsum, wmax := 0.0, 0.0
	for _, c := range clients {
		ops += c.ops
	}
	for _, s := range st.shards {
		drains += s.drains
		drec += s.drainedRec
		dropped += s.dropped
		reins += s.reinsert
		rehash += s.rehashes
		waves += s.waves
		gets += s.gets
		hits += s.getHits
		wsum += s.waveSumUs
		if s.waveMaxUs > wmax {
			wmax = s.waveMaxUs
		}
	}
	rate := 0.0
	if seconds > 0 {
		rate = float64(ops) / seconds / 1e6
	}
	live, mapn := 0, 0
	for _, s := range st.shards {
		live += s.liveEntries()
		mapn += s.keys()
	}
	printMem(st, "end")
	fmt.Printf("# keys: live_n=%d map_n=%d\n", live, mapn)
	st.trimTo(trimKeys)
	if !useFree {
		runtime.GC()
		debug.FreeOSMemory()
	}
	printMem(st, "trim")
	fmt.Printf("# keys: trim live_n=%d map_n=%d target=%d\n",
		st.liveEntries(), st.mapKeys(), trimKeys)
	fmt.Printf("# ops=%d (%.2fM/s)  get_hits=%d/%d\n", ops, rate, hits, gets)
	fmt.Printf("# expire: dropped=%d drained_recs=%d drains=%d reinserted=%d\n",
		dropped, drec, drains, reins)
	fmt.Printf("# index: rehashes=%d  (map grow, not expire)\n", rehash)
	fmt.Printf("# waves=%d mean=%.1f us max=%.0f us (hold %.0f us / sample %d, budget %d recs)\n",
		waves, mean(wsum, waves), wmax, waveHoldUs, waveHoldSample, wave)
	for i, s := range st.shards {
		fmt.Printf("#   shard %d: ops=%d keys=%d waves=%d drains=%d "+
			"never_live=%d never_dead=%d\n",
			i, s.ops, s.keys(), s.waves, s.drains,
			s.never.cur.liveBytes, s.never.cur.deadBytes)
	}
}

func mean(sum float64, n int) float64 {
	if n == 0 {
		return 0
	}
	return sum / float64(n)
}
