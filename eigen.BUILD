package(default_visibility = ["//visibility:public"])

archive_dir = "eigen-eigen-5651786d5e59"

cc_library(
    name = "eigen",
    hdrs = glob([archive_dir+"/**/*.h", archive_dir+"/unsupported/Eigen/CXX11/*", archive_dir+"/Eigen/*"]),
    includes = [ archive_dir ],
    visibility = ["//visibility:public"],
)

