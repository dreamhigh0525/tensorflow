# TensorFlow external dependencies that can be loaded in WORKSPACE files.

# If TensorFlow is linked as a submodule, path_prefix is TensorFlow's directory
# within the workspace (e.g. "tensorflow/"), and tf_repo_name is the name of the
# local_repository rule (e.g. "@tf").
def tf_workspace(path_prefix = "", tf_repo_name = ""):
  native.new_http_archive(
    name = "gmock_archive",
    url = "https://archive.openswitch.net/gmock-1.7.0.zip",
    sha256 = "26fcbb5925b74ad5fc8c26b0495dfc96353f4d553492eb97e85a8a6d2f43095b",
    build_file = path_prefix + "google/protobuf/gmock.BUILD",
  )

  native.new_http_archive(
    name = "eigen_archive",
    url = "https://bitbucket.org/eigen/eigen/get/a5e9085a94e8.tar.gz",
    sha256 = "967126237829c7c87abb6cd0e13a5a235b0377d51575522c390b9486aed13e71",
    build_file = path_prefix + "eigen.BUILD",
  )

  native.bind(
    name = "gtest",
    actual = "@gmock_archive//:gtest",
  )

  native.bind(
    name = "gtest_main",
    actual = "@gmock_archive//:gtest_main",
  )

  native.git_repository(
    name = "re2",
    remote = "https://github.com/google/re2.git",
    commit = "791beff",
  )

  native.git_repository(
    name = "gemmlowp",
    remote = "https://github.com/google/gemmlowp.git",
    commit = "96d3acab46fbb03855ca22c2ee2bb9831ac8c83c",
  )

  native.new_http_archive(
    name = "farmhash_archive",
    url = "https://github.com/google/farmhash/archive/34c13ddfab0e35422f4c3979f360635a8c050260.zip",
    sha256 = "e3d37a59101f38fd58fb799ed404d630f0eee18bfc2a2433910977cc8fea9c28",
    build_file = path_prefix + "farmhash.BUILD",
  )

  native.bind(
    name = "farmhash",
    actual = "@farmhash//:farmhash",
  )

  native.git_repository(
    name = "highwayhash",
    remote = "https://github.com/google/highwayhash.git",
    commit = "be5edafc2e1a455768e260ccd68ae7317b6690ee",
    init_submodules = True,
  )

  native.new_http_archive(
    name = "jpeg_archive",
    url = "http://www.ijg.org/files/jpegsrc.v9a.tar.gz",
    sha256 = "3a753ea48d917945dd54a2d97de388aa06ca2eb1066cbfdc6652036349fe05a7",
    build_file = path_prefix + "jpeg.BUILD",
  )

  native.new_http_archive(
    name = "png_archive",
    url = "https://github.com/glennrp/libpng/archive/v1.2.53.zip",
    sha256 = "c35bcc6387495ee6e757507a68ba036d38ad05b415c2553b3debe2a57647a692",
    build_file = path_prefix + "png.BUILD",
  )

  native.new_http_archive(
    name = "six_archive",
    url = "https://pypi.python.org/packages/source/s/six/six-1.10.0.tar.gz#md5=34eed507548117b2ab523ab14b2f8b55",
    sha256 = "105f8d68616f8248e24bf0e9372ef04d3cc10104f1980f54d57b2ce73a5ad56a",
    build_file = path_prefix + "six.BUILD",
  )

  native.bind(
    name = "six",
    actual = "@six_archive//:six",
  )

  # grpc expects //external:protobuf_clib and //external:protobuf_compiler
  # to point to the protobuf's compiler library.
  native.bind(
    name = "protobuf_clib",
    actual = tf_repo_name + "//google/protobuf:protoc_lib",
  )

  native.bind(
    name = "protobuf_compiler",
    actual = tf_repo_name + "//google/protobuf:protoc_lib",
  )

  native.git_repository(
    name = "grpc",
    commit = "2bc7d80",
    init_submodules = True,
    remote = "https://github.com/grpc/grpc.git",
  )

  # protobuf expects //external:grpc_cpp_plugin to point to grpc's
  # C++ plugin code generator.
  native.bind(
    name = "grpc_cpp_plugin",
    actual = "@grpc//:grpc_cpp_plugin",
  )

  native.bind(
    name = "grpc_lib",
    actual = "@grpc//:grpc++_unsecure",
  )

  native.new_git_repository(
    name = "jsoncpp_git",
    remote = "https://github.com/open-source-parsers/jsoncpp.git",
    commit = "11086dd6a7eba04289944367ca82cea71299ed70",
    build_file = path_prefix + "jsoncpp.BUILD",
  )

  native.bind(
    name = "jsoncpp",
    actual = "@jsoncpp_git//:jsoncpp",
  )

  native.new_git_repository(
    name = "boringssl_git",
    commit = "e72df93461c6d9d2b5698f10e16d3ab82f5adde3",
    remote = "https://boringssl.googlesource.com/boringssl",
    build_file = path_prefix + "boringssl.BUILD",
  )
