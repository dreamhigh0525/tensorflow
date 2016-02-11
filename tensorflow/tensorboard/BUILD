# Description:
# TensorBoard, a dashboard for investigating TensorFlow

package(default_visibility = ["//third_party/tensorflow:internal"])

filegroup(
    name = "tensorboard_frontend",
    srcs = [
        "dist/index.html",
        "dist/tf-tensorboard.html",
        "//third_party/tensorflow/tensorboard/bower:bower",
        "TAG",
    ] + glob(["lib/**/*"]),
)

py_library(
    name = "tensorboard_handler",
    srcs = ["backend/tensorboard_handler.py"],
    deps = [
        ":float_wrapper",
        "//third_party/tensorflow/python:platform",
        "//third_party/tensorflow/python:summary",
    ],
    srcs_version = "PY2AND3",
)

py_library(
    name = "float_wrapper",
    srcs = ["backend/float_wrapper.py"],
    srcs_version = "PY2AND3",
)

py_test(
    name = "float_wrapper_test",
    size = "small",
    srcs = ["backend/float_wrapper_test.py"],
    deps = [
        ":float_wrapper",
        "//third_party/tensorflow/python:platform_test",
    ],
    srcs_version = "PY2AND3",
)

py_binary(
    name = "tensorboard",
    srcs = ["backend/tensorboard.py"],
    data = [":tensorboard_frontend"],
    deps = [
        ":tensorboard_handler",
        "//third_party/tensorflow/python:platform",
        "//third_party/tensorflow/python:summary",
    ],
    srcs_version = "PY2AND3",
)
