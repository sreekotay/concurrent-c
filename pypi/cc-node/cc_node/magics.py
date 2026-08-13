"""IPython magics for cc-node. Imported only from load_ipython_extension."""
from IPython.core.magic import Magics, line_cell_magic, line_magic, magics_class
from IPython.core.magic_arguments import argument, magic_arguments, parse_argstring

from . import JsError, kernel, reset_kernel


def _bind_map(names, ns):
    out = {}
    for item in names or []:
        for name in item.split(","):
            name = name.strip()
            if not name:
                continue
            if name not in ns:
                raise JsError(
                    "cc-node: --bind name %r is not in the namespace" % (name,))
            out[name] = ns[name]
    return out


@magics_class
class CcNodeMagics(Magics):

    @magic_arguments()
    @argument("-b", "--bind", action="append", default=[],
              help="Python names to publish on globalThis")
    @argument("-t", "--to", default=None,
              help="Store the result in this Python name")
    @argument("code", nargs="*", help="JS source (line magic)")
    @line_cell_magic
    def js(self, line, cell=None):
        """Eval JavaScript on the kernel-scoped domain.

        Line:  %js 1 + 1
        Cell:  %%js   then a body; last expression is the result.
        Bind:  %%js -b xs -b n     (wire types only; no pickle fallback)
        Store: %%js -t out
        """
        args = parse_argstring(self.js, line)
        body = cell if cell is not None else ""
        line_code = " ".join(args.code)
        src = body if str(body).strip() else line_code
        if not str(src).strip():
            raise JsError("cc-node: empty %js / %%js")
        ns = self.shell.user_ns
        bindings = _bind_map(args.bind, ns)
        result = kernel().eval_cell(src, bindings or None)
        if args.to:
            ns[args.to] = result
        return result

    @line_magic
    def js_reset(self, line):
        """Destroy the kernel-scoped Node child. Next %%js spawns again."""
        reset_kernel()

    @line_magic
    def js_stats(self, line):
        """Handle-table size of the kernel domain (spawns if needed)."""
        return kernel().stats()


def load_ipython_extension(ip):
    ip.register_magics(CcNodeMagics)
