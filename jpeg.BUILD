SOURCES = [
    "jaricom.c",
    "jcapimin.c",
    "jcapistd.c",
    "jcarith.c",
    "jccoefct.c",
    "jccolor.c",
    "jcdctmgr.c",
    "jchuff.c",
    "jcinit.c",
    "jcmainct.c",
    "jcmarker.c",
    "jcmaster.c",
    "jcomapi.c",
    "jcparam.c",
    "jcprepct.c",
    "jcsample.c",
    "jctrans.c",
    "jdarith.c",
    "jdapimin.c",
    "jdapistd.c",
    "jdatadst.c",
    "jdatasrc.c",
    "jdcoefct.c",
    "jdcolor.c",
    "jddctmgr.c",
    "jdhuff.c",
    "jdinput.c",
    "jdmainct.c",
    "jdmarker.c",
    "jdmaster.c",
    "jdmerge.c",
    "jdpostct.c",
    "jdsample.c",
    "jdtrans.c",
    "jerror.c",
    "jfdctflt.c",
    "jfdctfst.c",
    "jfdctint.c",
    "jidctflt.c",
    "jidctfst.c",
    "jidctint.c",
    "jmemmgr.c",
    "jmemnobs.c",
    "jquant1.c",
    "jquant2.c",
    "jutils.c",
]

HEADERS = [
    "cderror.h",
    "cdjpeg.h",
    "jconfig.h",
    "jdct.h",
    "jerror.h",
    "jinclude.h",
    "jmemsys.h",
    "jmorecfg.h",
    "jpegint.h",
    "jpeglib.h",
    "jversion.h",
    "transupp.h",
]

prefix_dir = "jpeg-9a"

genrule(
    name = "configure",
    srcs = glob(
        ["**/*"],
        exclude = [prefix_dir + "/jconfig.h"],
    ),
    outs = [prefix_dir + "/jconfig.h"],
    cmd = "pushd external/jpeg_archive/%s; workdir=$$(mktemp -d -t tmp.XXXXXXXXXX); cp -a * $$workdir; pushd $$workdir; ./configure; popd; popd; cp $$workdir/jconfig.h $(@D); rm -rf $$workdir;" % prefix_dir,
)

cc_library(
    name = "jpeg",
    srcs = [prefix_dir + "/" + source for source in SOURCES],
    hdrs = glob(["**/*.h"]) + [":configure"],
    includes = [prefix_dir],
    visibility = ["//visibility:public"],
)
