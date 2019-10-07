/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/experimental/micro/examples/gesture_recognition/angle_micro_features_data.h"

const int g_angle_micro_f2e59fea_nohash_1_length = 128;
const int g_angle_micro_f2e59fea_nohash_1_dim = 3;
// Raw accelerometer data with a sample rate of 25Hz
const float g_angle_micro_f2e59fea_nohash_1_data[] = {
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,    0.0,
    0.0,    0.0,    0.0,    -766.0, 132.0,  709.0,  -751.0, 249.0,  659.0,
    -714.0, 314.0,  630.0,  -709.0, 244.0,  623.0,  -707.0, 230.0,  659.0,
    -704.0, 202.0,  748.0,  -714.0, 219.0,  728.0,  -722.0, 239.0,  710.0,
    -744.0, 116.0,  612.0,  -753.0, -49.0,  570.0,  -748.0, -279.0, 527.0,
    -668.0, -664.0, 592.0,  -601.0, -635.0, 609.0,  -509.0, -559.0, 606.0,
    -286.0, -162.0, 536.0,  -255.0, -144.0, 495.0,  -209.0, -85.0,  495.0,
    6.0,    416.0,  698.0,  -33.0,  304.0,  1117.0, -82.0,  405.0,  1480.0,
    -198.0, 1008.0, 1908.0, -229.0, 990.0,  1743.0, -234.0, 934.0,  1453.0,
    -126.0, 838.0,  896.0,  -78.0,  792.0,  911.0,  -27.0,  741.0,  918.0,
    114.0,  734.0,  960.0,  135.0,  613.0,  959.0,  152.0,  426.0,  1015.0,
    106.0,  -116.0, 1110.0, 63.0,   -314.0, 1129.0, -12.0,  -486.0, 1179.0,
    -118.0, -656.0, 1510.0, -116.0, -558.0, 1553.0, -126.0, -361.0, 1367.0,
    -222.0, -76.0,  922.0,  -210.0, -26.0,  971.0,  -194.0, 50.0,   1053.0,
    -178.0, 72.0,   1082.0, -169.0, 100.0,  1073.0, -162.0, 133.0,  1050.0,
    -156.0, 226.0,  976.0,  -154.0, 323.0,  886.0,  -130.0, 240.0,  1154.0,
    -116.0, 124.0,  916.0,  -132.0, 124.0,  937.0,  -153.0, 115.0,  981.0,
    -184.0, 94.0,   962.0,  -177.0, 85.0,   1017.0, -173.0, 92.0,   1027.0,
    -168.0, 158.0,  1110.0, -181.0, 101.0,  1030.0, -180.0, 139.0,  1054.0,
    -152.0, 10.0,   1044.0, -169.0, 74.0,   1007.0,
};
