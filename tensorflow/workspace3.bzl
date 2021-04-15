"""TensorFlow workspace initialization. Consult the WORKSPACE on how to use it."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def workspace():
    http_archive(
        name = "io_bazel_rules_closure",
        sha256 = "5b00383d08dd71f28503736db0500b6fb4dda47489ff5fc6bed42557c07c6ba9",
        strip_prefix = "rules_closure-308b05b2419edb5c8ee0471b67a40403df940149",
        urls = [
            "https://storage.googleapis.com/mirror.tensorflow.org/github.com/bazelbuild/rules_closure/archive/308b05b2419edb5c8ee0471b67a40403df940149.tar.gz",
            "https://github.com/bazelbuild/rules_closure/archive/308b05b2419edb5c8ee0471b67a40403df940149.tar.gz",  # 2019-06-13
        ],
    )

    http_archive(
        name = "tf_toolchains",
        sha256 = "de1d491188016318b3c56f56c483ae704d27031ff795a092e2a7fa2d6034a884",
        strip_prefix = "toolchains-1.1.11",
        urls = [
            "http://mirror.tensorflow.org/github.com/tensorflow/toolchains/archive/v1.1.11.tar.gz",
            "https://github.com/tensorflow/toolchains/archive/v1.1.11.tar.gz",
        ],
    )

    http_archive(
        name = "tf_runtime",
        sha256 = "2bf3174ac0d4e19163000b61b87540cc836240dfd44412c70fd51d487c888806",
        strip_prefix = "runtime-0740fb6328285b9495332b5fe075d28173b58d6f",
        urls = [
            "http://mirror.tensorflow.org/github.com/tensorflow/runtime/archive/0740fb6328285b9495332b5fe075d28173b58d6f.tar.gz",
            "https://github.com/tensorflow/runtime/archive/0740fb6328285b9495332b5fe075d28173b58d6f.tar.gz",
        ],
    )

# Alias so it can be loaded without assigning to a different symbol to prevent
# shadowing previous loads and trigger a buildifier warning.
tf_workspace3 = workspace
