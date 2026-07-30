# Homebrew formula for Concurrent-C compiler (ccc).
#
# Install from this repo (the formula fetches submodules itself):
#   git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
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
    # Fetches the tcc and xjb submodules the build needs (liblfds is optional).
    system "./scripts/fetch_submodules.sh"
    system "./scripts/apply_tcc_patches.sh"
    cd "third_party/tcc" do
      system "./configure", "--config-cc_ext"
      system "make", "-j#{ENV.make_jobs}"
    end

    # Build the compiler (release).
    system "make", "cc", "BUILD=release", "-j#{ENV.make_jobs}"

    # Install to Homebrew prefix. Driver resolves paths from executable location.
    system "make", "install", "PREFIX=#{prefix}"
    system "make", "install-check", "PREFIX=#{prefix}"
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
