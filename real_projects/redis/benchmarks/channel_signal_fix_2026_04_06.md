# Channel Signal Benchmark Fix 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Idiomatic port: `6389`
- Upstream port: `6390`
- Baseline source: `benchmark_baseline.txt`

## Summary vs Baseline

| Case | Baseline idiomatic rps | New idiomatic rps | Delta | Baseline upstream rps | New upstream rps | New idiomatic/upstream | Baseline idiomatic p50 | New idiomatic p50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 218240.95 | 166744.30 | 0.764x | 211895.10 | 207804.97 | 0.802x | 0.127 | 0.156 |
| `GET P=1` | 220589.90 | 206384.75 | 0.936x | 217867.68 | 207282.75 | 0.996x | 0.127 | 0.135 |
| `SET P=16` | 2873674.42 | 2543546.92 | 0.885x | 2373438.58 | 2247948.00 | 1.131x | 0.226 | 0.247 |
| `GET P=16` | 2958748.58 | 2777778.00 | 0.939x | 2843606.92 | 2191994.37 | 1.267x | 0.151 | 0.212 |
| `INCR P=16` | 1805532.59 | 2346904.58 | 1.300x | 2700274.42 | 2543016.00 | 0.923x | 0.271 | 0.287 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 110497.24 requests per second, p50=0.199 msec`
- `GET P=1`: `GET: 210084.03 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2597402.50 requests per second, p50=0.263 msec`
- `GET P=16`: `GET: 2777778.00 requests per second, p50=0.207 msec`
- `INCR P=16`: `INCR: 2469135.75 requests per second, p50=0.279 msec`

### upstream
- `SET P=1`: `SET: 211864.41 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 209424.09 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2298850.75 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2739726.00 requests per second, p50=0.231 msec`
- `INCR P=16`: `INCR: 2597402.50 requests per second, p50=0.255 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 207253.89 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 207253.89 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2564102.50 requests per second, p50=0.239 msec`
- `GET P=16`: `GET: 2777778.00 requests per second, p50=0.223 msec`
- `INCR P=16`: `INCR: 2298850.75 requests per second, p50=0.303 msec`

### upstream
- `SET P=1`: `SET: 207468.88 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 208550.58 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2247191.00 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 1169590.62 requests per second, p50=0.471 msec`
- `INCR P=16`: `INCR: 2500000.00 requests per second, p50=0.255 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 182481.77 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 201816.34 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2469135.75 requests per second, p50=0.239 msec`
- `GET P=16`: `GET: 2777778.00 requests per second, p50=0.207 msec`
- `INCR P=16`: `INCR: 2272727.25 requests per second, p50=0.279 msec`

### upstream
- `SET P=1`: `SET: 204081.62 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 203873.59 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2197802.25 requests per second, p50=0.303 msec`
- `GET P=16`: `GET: 2666666.50 requests per second, p50=0.239 msec`
- `INCR P=16`: `INCR: 2531645.50 requests per second, p50=0.263 msec`

