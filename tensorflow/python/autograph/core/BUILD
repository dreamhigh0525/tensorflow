load("//tensorflow:strict.default.bzl", "py_strict_library", "py_strict_test")
load("//tensorflow:pytype.default.bzl", "pytype_strict_library")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    licenses = ["notice"],
)

py_strict_library(
    name = "test_lib",
    srcs = [
        "converter_testing.py",
    ],
    srcs_version = "PY3",
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        ":config",
        ":converter",
        "//tensorflow/python/autograph/impl",
        "//tensorflow/python/framework:ops",
        "//tensorflow/python/platform:client_testlib",
    ],
)

py_strict_library(
    name = "ag_ctx",
    srcs = ["ag_ctx.py"],
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        "//tensorflow/python/autograph/utils:ag_logging",
        "//tensorflow/python/util:tf_export",
    ],
)

py_strict_library(
    name = "unsupported_features_checker",
    srcs = ["unsupported_features_checker.py"],
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        "//tensorflow/python/autograph/pyct",
        "@gast_archive//:gast",
    ],
)

py_strict_library(
    name = "config",
    srcs = ["config.py"],
    visibility = ["//tensorflow:__subpackages__"],
    deps = [":config_lib"],
)

py_strict_library(
    name = "config_lib",
    srcs = ["config_lib.py"],
    visibility = ["//tensorflow:__subpackages__"],
)

py_strict_library(
    name = "function_wrappers",
    srcs = ["function_wrappers.py"],
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        ":ag_ctx",
        ":converter",
        "//tensorflow/python:framework_ops",
        "//tensorflow/python/autograph/operators",
        "//tensorflow/python/framework:auto_control_deps",
        "//tensorflow/python/framework:tensor_util",
        "//tensorflow/python/util:nest",
    ],
)

py_strict_library(
    name = "converter",
    srcs = ["converter.py"],
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        "//tensorflow/python/autograph/pyct",
        "//tensorflow/python/util:tf_export",
    ],
)

py_strict_test(
    name = "converter_test",
    srcs = ["converter_test.py"],
    python_version = "PY3",
    srcs_version = "PY3",
    deps = [
        ":converter",
        ":test_lib",
        "//tensorflow/python:client_testlib",
        "//tensorflow/python/autograph/pyct",
    ],
)

py_strict_test(
    name = "function_wrappers_test",
    srcs = ["function_wrappers_test.py"],
    python_version = "PY3",
    srcs_version = "PY3",
    deps = [
        ":converter",
        ":function_wrappers",
        "//tensorflow/python:client_testlib",
        "//tensorflow/python:variables",
        "//tensorflow/python/eager:context",
        "//tensorflow/python/framework:constant_op",
    ],
)

pytype_strict_library(
    name = "config_oss",
    srcs = ["config.oss.py"],
    deps = [":config_lib"],
)
