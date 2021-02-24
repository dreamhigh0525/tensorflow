# Copyright 2017 The TensorFlow Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Utilities for defining TensorFlow Bazel dependencies."""

def _get_env_var(ctx, name):
    if name in ctx.os.environ:
        return ctx.os.environ[name]
    else:
        return None

# Checks if we should use the system lib instead of the bundled one
def _use_system_lib(ctx, name):
    syslibenv = _get_env_var(ctx, "TF_SYSTEM_LIBS")
    if not syslibenv:
        return False
    return name in [n.strip() for n in syslibenv.split(",")]

def _get_link_dict(ctx, link_files, build_file):
    if build_file:
        # Use BUILD.bazel because it takes precedence over BUILD.
        link_files = dict(link_files, **{build_file: "BUILD.bazel"})
    return {ctx.path(v): Label(k) for k, v in link_files.items()}

def _tf_http_archive_impl(ctx):
    if len(ctx.attr.urls) < 2 or "mirror.tensorflow.org" not in ctx.attr.urls[0]:
        fail("tf_http_archive(urls) must have redundant URLs. The " +
             "mirror.tensorflow.org URL must be present and it must come first. " +
             "Even if you don't have permission to mirror the file, please " +
             "put the correctly formatted mirror URL there anyway, because " +
             "someone will come along shortly thereafter and mirror the file.")

    # Construct all labels early on to prevent rule restart. We want the
    # attributes to be strings instead of labels because they refer to files
    # in the TensorFlow repository, not files in repos depending on TensorFlow.
    # See also https://github.com/bazelbuild/bazel/issues/10515.
    link_dict = _get_link_dict(ctx, ctx.attr.link_files, ctx.attr.build_file)

    if _use_system_lib(ctx, ctx.attr.name):
        link_dict.update(_get_link_dict(
            ctx = ctx,
            link_files = ctx.attr.system_link_files,
            build_file = ctx.attr.system_build_file,
        ))
    else:
        patch_file = ctx.attr.patch_file
        patch_file = Label(patch_file) if patch_file else None
        ctx.download_and_extract(
            url = ctx.attr.urls,
            sha256 = ctx.attr.sha256,
            type = ctx.attr.type,
            stripPrefix = ctx.attr.strip_prefix,
        )
        if patch_file:
            ctx.patch(patch_file, strip = 1)

    for path, label in link_dict.items():
        ctx.delete(path)
        ctx.symlink(label, path)

_tf_http_archive = repository_rule(
    implementation = _tf_http_archive_impl,
    attrs = {
        "sha256": attr.string(mandatory = True),
        "urls": attr.string_list(mandatory = True),
        "strip_prefix": attr.string(),
        "type": attr.string(),
        "patch_file": attr.string(),
        "build_file": attr.string(),
        "system_build_file": attr.string(),
        "link_files": attr.string_dict(),
        "system_link_files": attr.string_dict(),
    },
    environ = ["TF_SYSTEM_LIBS"],
)

def tf_http_archive(name, sha256, urls, **kwargs):
    """Downloads and creates Bazel repos for dependencies.

    This is a swappable replacement for both http_archive() and
    new_http_archive() that offers some additional features. It also helps
    ensure best practices are followed.

    File arguments are relative to the TensorFlow repository by default. Dependent
    repositories that use this rule should refer to files either with absolute
    labels (e.g. '@foo//:bar') or from a label created in their repository (e.g.
    'str(Label("//:bar"))').
    """
    if native.existing_rule(name):
        print("\n\033[1;33mWarning:\033[0m skipping import of repository '" +
              name + "' because it already exists.\n")
        return

    _tf_http_archive(
        name = name,
        sha256 = sha256,
        urls = urls,
        **kwargs
    )

# Introduced for go/tfbr-thirdparty, now alias of tf_http_archive.
# TODO(csigg): Update call sites and remove.
third_party_http_archive = tf_http_archive
