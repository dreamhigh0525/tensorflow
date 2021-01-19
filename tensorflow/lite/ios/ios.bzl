"""TensorFlow Lite Build Configurations for iOS"""

# Placeholder for Google-internal load statements.
load("@build_bazel_rules_apple//apple:ios.bzl", "ios_static_framework")

TFL_MINIMUM_OS_VERSION = "9.0"

# Default tags for filtering iOS targets. Targets are restricted to Apple platforms.
TFL_DEFAULT_TAGS = [
    "apple",
]

# Following sanitizer tests are not supported by iOS test targets.
TFL_DISABLED_SANITIZER_TAGS = [
    "noasan",
    "nomsan",
    "notsan",
]

# iOS static framework with symbol allowlist. Exported C++ symbols might cause
# symbol collision with other libraries. List of symbols to allowlist can be
# generated by running `nm -m -g FRAMEWORK_LIBRARY | grep _TfLite` for framework
# built with `ios_static_framework` rule.
def tflite_ios_static_framework(
        name,
        bundle_name,
        allowlist_symbols_file,
        exclude_resources = True,
        **kwargs):
    """TFLite variant of ios_static_framework with symbol hiding.

    Args:
      name: The name of the target.
      bundle_name: The name to give to the framework bundle, without the
          ".framework" extension. If omitted, the target's name will be used.
      allowlist_symbols_file: a file including a list of allowed symbols,
          one symbol per line.
      exclude_resources: Indicates whether resources should be excluded from the
          bundle. This can be used to avoid unnecessarily bundling resources if
          the static framework is being distributed in a different fashion, such
          as a Cocoapod.
      **kwargs: Pass-through arguments.
    """

    preprocessed_name = "Preprocessed_" + name
    ios_static_framework(
        name = preprocessed_name,
        bundle_name = bundle_name,
        exclude_resources = exclude_resources,
        **kwargs
    )

    framework_target = ":{}.zip".format(preprocessed_name)

    srcs = [
        framework_target,
        allowlist_symbols_file,
    ]
    cmd = ("INPUT_FRAMEWORK=\"$(location " + framework_target + ")\" " +
           "BUNDLE_NAME=\"" + bundle_name + "\" " +
           "ALLOWLIST_FILE_PATH=\"$(location " + allowlist_symbols_file + ")\" " +
           "EXTRACT_SCRIPT_PATH=\"$(location //tensorflow/lite/ios:extract_object_files_main)\" " +
           "OUTPUT=\"$(OUTS)\" " +
           "\"$(location //tensorflow/lite/ios:hide_symbols_with_allowlist)\"")

    native.genrule(
        name = name,
        srcs = srcs,
        outs = [name + ".zip"],
        cmd = cmd,
        tools = [
            "//tensorflow/lite/ios:extract_object_files_main",
            "//tensorflow/lite/ios:hide_symbols_with_allowlist",
        ],
    )

# When the static framework is built with bazel, the all header files are moved
# to the "Headers" directory with no header path prefixes. This auxiliary rule
# is used for stripping the path prefix of header inclusions paths from the
# provided headers.
def strip_common_include_path_prefix(name, hdr_labels, prefix = ""):
    """Create modified header files with the inclusion path prefixes removed.

    Args:
      name: The name to be used as a prefix to the generated genrules.
      hdr_labels: List of header labels to strip out the include path. Each
          label must end with a colon followed by the header file name.
      prefix: Optional prefix path to prepend to the final inclusion path.
    """

    for hdr_label in hdr_labels:
        hdr_filename = hdr_label.split(":")[-1]
        hdr_basename = hdr_filename.split(".")[0]

        native.genrule(
            name = "{}_{}".format(name, hdr_basename),
            srcs = [hdr_label],
            outs = [hdr_filename],
            cmd = """
            sed -E 's|#include ".*/([^/]+\\.h)"|#include "{}\\1"|g'\
            "$(location {})"\
            > "$@"
            """.format(prefix, hdr_label),
        )
