"""Provides the repository macro to import LLVM."""

load("//third_party:repo.bzl", "tf_http_archive")

def repo(name):
    """Imports LLVM."""
    LLVM_COMMIT = "24a37a396a9bd6b73b05b4eafce8b87e7a748cf9"
    LLVM_SHA256 = "05017838bf7a8594855367a86f1632703bd5088ba4bffa36e1c5247badd658f9"

    tf_http_archive(
        name = name,
        sha256 = LLVM_SHA256,
        strip_prefix = "llvm-project-{commit}".format(commit = LLVM_COMMIT),
        urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/github.com/llvm/llvm-project/archive/{commit}.tar.gz".format(commit = LLVM_COMMIT),
            "https://github.com/llvm/llvm-project/archive/{commit}.tar.gz".format(commit = LLVM_COMMIT),
        ],
        build_file = "//third_party/llvm:llvm.BUILD",
        patch_file = ["//third_party/llvm:macos_build_fix.patch"],
        link_files = {"//third_party/llvm:run_lit.sh": "mlir/run_lit.sh"},
    )
