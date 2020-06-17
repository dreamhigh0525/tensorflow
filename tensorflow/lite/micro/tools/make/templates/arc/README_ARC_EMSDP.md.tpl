# TensorFlow Lite Micro ARC Make Project for EM SDP Board.

This folder has been autogenerated by TensorFlow, and contains source, header, and project files needed to build a single TensorFlow Lite Micro target using make tool and and a Synopsys DesignWare ARC processor compatible toolchain, specifically the ARC MetaWare Development Toolkit (MWDT).  

This project has been generated for the ARC EM Software Development Platform (EM SDP). The built application can be run only on this platform.

See
[tensorflow/lite/micro](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/lite/micro)
for details on how projects like this can be generated from the main source tree.

## Usage

See [ARC EM Software Development Platform](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/lite/micro/tools/make/targets/arc/README.md#ARC-EM-Software-Development-Platform-ARC-EM-SDP) section for more detailed information on requirements and usage of this project. 

The Makefile contains all the information on building and running the project. One can modify it to satisfy specific needs. Next actions are available out of the box. You may need to adjust the following commands in order to use the appropriate make tool available in your environment, ie: `make` or `gmake`:

1. Build the application.

       make app 

2. Build the application passing additional flags to compiler.

       make app EXT_CFLAGS=[additional compiler flags]

3. Build the application and stripout TFLM reference kernel fallback implementations in order to reduce code size. This only has an effect in case the project was generated with MLI support. See more info in [EmbARC MLI Library Based Optimizations](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/lite/micro/kernels/arc_mli/README.md). `false` is the default value. 

       make app MLI_ONLY=[true|false]

4. Delete all artifacts created during build.

       make clean

5. Run the application with the nSIM simulator in console mode.

       make run 

6. Load the application and open MetaWare Debugger GUI for further execution/debugging. 	

       make debug 

7. Generate necessary artefacts for self-booting execution from flash. See [reference to Run the application on the board from the micro SD card](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/lite/micro/tools/make/targets/arc/README.md#Run-the-Application-on-the-Board-from-the-microSD-Card). 	

       make flash


## License

TensorFlow's code is covered by the Apache2 License included in the repository, and third party dependencies are covered by their respective licenses, in the third_party folder of this package.
