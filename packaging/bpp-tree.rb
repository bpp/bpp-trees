# Seed copy of the Homebrew formula. The canonical copy lives in
# bpp/homebrew-tap (Formula/bpp-tree.rb); add this there once, and the
# release workflow's bump-homebrew job keeps its url + sha256 current on
# every tagged release. The sha256 below is a placeholder for the initial
# seed -- the first `v*` tag push overwrites it with the real checksum.
class BppTree < Formula
  desc "Compile join formulas into BPP species trees (MSC / MSC-M / MSC-I)"
  homepage "https://github.com/bpp/bpp-trees"
  url "https://github.com/bpp/bpp-trees/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "AGPL-3.0-or-later"
  head "https://github.com/bpp/bpp-trees.git", branch: "main"

  def install
    system "make"
    bin.install "bpp-tree"
    pkgshare.install "examples"
    doc.install "README.md"
  end

  test do
    assert_match "bpp-tree", shell_output("#{bin}/bpp-tree --version")
    # A join formula compiles to a Newick species tree.
    out = shell_output("#{bin}/bpp-tree #{pkgshare}/examples/primates.joins")
    assert_match "macaque", out
  end
end
