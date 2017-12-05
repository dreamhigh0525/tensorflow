# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for DecodeRaw op from parsing_ops."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import gzip
import sys
import zlib

from six import StringIO

from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import parsing_ops
from tensorflow.python.platform import test


class DecodeCompressedOpTest(test.TestCase):

  def _compress(self, bytes, compression_type):
    if compression_type == "ZLIB":
      return zlib.compress(bytes)
    else:
      out = StringIO()
      with gzip.GzipFile(fileobj=out, mode="w") as f:
        f.write(bytes)
      return out.getvalue()

  def testDecompress(self):
    for compression_type in ["ZLIB", "GZIP"]:
      with self.test_session():
        in_bytes = array_ops.placeholder(dtypes.string, shape=[2])
        decompressed = parsing_ops.decode_compressed(
            in_bytes, compression_type=compression_type)
        self.assertEqual([2], decompressed.get_shape().as_list())

        result = decompressed.eval(
            feed_dict={in_bytes: [self._compress("AaAA", compression_type),
                                  self._compress("bBbb", compression_type)]})
        self.assertAllEqual(["AaAA", "bBbb"], result)

  def testDecompressWithRaw(self):
    for compression_type in ["ZLIB", "GZIP"]:
      with self.test_session():
        in_bytes = array_ops.placeholder(dtypes.string, shape=[None])
        decompressed = parsing_ops.decode_compressed(
            in_bytes, compression_type=compression_type)
        decode = parsing_ops.decode_raw(decompressed, out_type=dtypes.int16)

        result = decode.eval(
            feed_dict={in_bytes: [self._compress("AaBC", compression_type)]})
        self.assertAllEqual(
            [[ord("A") + ord("a") * 256, ord("B") + ord("C") * 256]], result)


if __name__ == "__main__":
  test.main()
