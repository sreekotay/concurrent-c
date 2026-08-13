'use strict';
/* Long-lived child: inherited stdout is often a pipe (Jupyter, nohup).
 * Node then block-buffers console.log until the buffer fills or exit.
 * Blocking writes make print land in the cell as it happens. TTY is
 * already blocking — this is a no-op there. */
function block(s) {
  try {
    if (s && s._handle && typeof s._handle.setBlocking === 'function')
      s._handle.setBlocking(true);
  } catch (e) { /* ignore */ }
}
block(process.stdout);
block(process.stderr);
