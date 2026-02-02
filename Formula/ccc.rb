# Homebrew formula for Concurrent-C compiler (ccc).
#
# Install from this repo (clone with submodules first):
#   git clone --recurse-submodules https://github.com/sreekotay/concurrent-c.git
#   cd concurrent-c && brew install --formula Formula/ccc.rb
#
# Or add as a tap and install head:
#   brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
#   brew install sreekotay/concurrent-c/ccc
#
# After install, `ccc` is on PATH. The driver finds runtime and headers from
# the installed layout (prefix/bin/ccc -> prefix/include, prefix/lib/ccc/runtime).

class Ccc < Formula
  desc "Concurrent-C compiler: C extension with async/await and structured concurrency"
  homepage "https://github.com/sreekotay/concurrent-c"
  license "MIT"
  head "https://github.com/sreekotay/concurrent-c.git", branch: "main"

  depends_on "make" => :build

  def install
    # TCC (Tiny C Compiler) is required; it's a submodule with local patches.
    system "git", "submodule", "update", "--init", "third_party/tcc"
    system "./scripts/apply_tcc_patches.sh"
    system "make", "-C", "third_party/tcc", "-j#{ENV.make_jobs}"

    # Build the compiler (release).
    system "make", "cc", "BUILD=release"

    # Install to Homebrew prefix. Driver resolves paths from executable location.
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    (testpath/"hello.ccs").write <<~CCS
      #include <ccc/cc_runtime.cch>
      #include <stdio.h>
      int main(void) {
        @nursery { spawn(() => printf("ok\\n")); }
        return 0;
      }
    CCS
    system bin/"ccc", "build", "run", "--out-dir", testpath/"out", "hello.ccs"
  end
end
