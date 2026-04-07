# Channel Signal Benchmark After Send-Wakeup Fix 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Idiomatic port: `6404`
- Upstream port: `6402`
- Baseline source: `benchmark_baseline.txt`

## Summary vs Baseline

| Case | Baseline idiomatic rps | New idiomatic rps | Delta | Baseline upstream rps | New upstream rps | New idiomatic/upstream | Baseline idiomatic p50 | New idiomatic p50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 218240.95 | 202284.08 | 0.927x | 211895.10 | 174249.00 | 1.161x | 0.127 | 0.135 |
| `GET P=1` | 220589.90 | 201928.51 | 0.915x | 217867.68 | 168935.84 | 1.195x | 0.127 | 0.135 |
| `SET P=16` | 2873674.42 | 2493414.00 | 0.868x | 2373438.58 | 2016533.33 | 1.236x | 0.226 | 0.266 |
| `GET P=16` | 2958748.58 | 1842760.54 | 0.623x | 2843606.92 | 2296296.25 | 0.802x | 0.151 | 0.212 |
| `INCR P=16` | 1805532.59 | 2080291.38 | 1.152x | 2700274.42 | 2283302.54 | 0.911x | 0.271 | 0.338 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 215749.73 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 216450.20 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2631579.00 requests per second, p50=0.271 msec`
- `GET P=16`: `GET: 2898550.75 requests per second, p50=0.207 msec`
- `INCR P=16`: `INCR: 2173913.00 requests per second, p50=0.319 msec`

### upstream
- `SET P=1`: `SET: 185701.02 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 182315.41 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2197802.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2666666.50 requests per second, p50=0.239 msec`
- `INCR P=16`: `INCR: 2469135.75 requests per second, p50=0.255 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 198609.73 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 199401.80 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2439024.25 requests per second, p50=0.271 msec`
- `GET P=16`: `GET: 32328.38 requests per second, p50=0.207 msec`
- `INCR P=16`: `INCR: 1869158.88 requests per second, p50=0.367 msec`

### upstream
- `SET P=1`: `SET: 183318.06 requests per second, p50=0.143 msec`
- `GET P=1`: `GET: 166389.34 requests per second, p50=0.143 msec`
- `SET P=16`: `SET: 2127659.75 requests per second, p50=0.303 msec`
- `GET P=16`: `GET: 2222222.25 requests per second, p50=0.255 msec`
- `INCR P=16`: `INCR: 2439024.25 requests per second, p50=0.263 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 192492.78 requests per second, p50=0.143 msec`
- `GET P=1`: `GET: 189933.53 requests per second, p50=0.143 msec`
- `SET P=16`: `SET: 2409638.75 requests per second, p50=0.255 msec`
- `GET P=16`: `GET: 2597402.50 requests per second, p50=0.223 msec`
- `INCR P=16`: `INCR: 2197802.25 requests per second, p50=0.327 msec`

### upstream
- `SET P=1`: `SET: 153727.91 requests per second, p50=0.151 msec`
- `GET P=1`: `GET: 158102.77 requests per second, p50=0.143 msec`
- `SET P=16`: `SET: 1724138.00 requests per second, p50=0.351 msec`
- `GET P=16`: `GET: 2000000.00 requests per second, p50=0.287 msec`
- `INCR P=16`: `INCR: 1941747.62 requests per second, p50=0.295 msec`

