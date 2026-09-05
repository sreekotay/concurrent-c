// Track B natural workload — structure from the lifetime design doc §6.
// Stub-ready: host implementations (txiki C / CC / Rust) supply
// setTimeout / clearTimeout / process-like shutdown later.

'use strict';

const epochs = 5;
const nestedDepth = 3;
const cancelEvery = 4;

function sleepMs(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function runEpoch(epoch) {
  const ids = [];
  let fired = 0;
  let cancelled = 0;
  let nested = 0;
  let threw = 0;

  for (let i = 0; i < 32; i++) {
    const id = setTimeout(() => {
      fired++;
      if (i % 7 === 0) {
        try {
          throw new Error('timer_throw_' + epoch + '_' + i);
        } catch (_) {
          threw++;
        }
      }
    }, (i % 5) + 1);
    ids.push(id);
    if (i % cancelEvery === 0) {
      clearTimeout(id);
      cancelled++;
    }
  }

  function nest(d) {
    if (d <= 0) return;
    setTimeout(() => {
      nested++;
      nest(d - 1);
    }, 1);
  }
  nest(nestedDepth);

  await sleepMs(50);

  return { epoch, fired, cancelled, nested, threw, outstanding: ids.length };
}

async function main() {
  const results = [];
  for (let e = 0; e < epochs; e++) {
    results.push(await runEpoch(e));
  }

  // Shutdown hooks: cancel remaining, then allow event loop to drain.
  if (typeof globalThis.__hostile_shutdown === 'function') {
    await globalThis.__hostile_shutdown();
  }

  if (typeof console !== 'undefined' && console.log) {
    console.log(JSON.stringify({ phase: 'timers_done', results }));
  }
  return results;
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { main, runEpoch };
}

main();
