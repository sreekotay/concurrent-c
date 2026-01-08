// Simple fixture to exercise cc build stub flow.
// The compiler currently copies input->output; the real check is via
// `cc build --dump-consts` to observe merged comptime consts.

int main() {
    return 0;
}

