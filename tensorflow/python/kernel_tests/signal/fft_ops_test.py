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
"""Tests for fft operations."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
from six.moves import xrange  # pylint: disable=redefined-builtin

from tensorflow.core.protobuf import config_pb2
from tensorflow.python.eager import context
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_spectral_ops
from tensorflow.python.ops import gradient_checker_v2
from tensorflow.python.ops import math_ops
from tensorflow.python.ops.signal import fft_ops
from tensorflow.python.platform import test

VALID_FFT_RANKS = (1, 2, 3)


class BaseFFTOpsTest(test.TestCase):

  def _compare(self, x, rank, fft_length=None, use_placeholder=False,
               rtol=1e-4, atol=1e-4):
    self._compare_forward(x, rank, fft_length, use_placeholder, rtol, atol)
    self._compare_backward(x, rank, fft_length, use_placeholder, rtol, atol)

  def _compare_forward(self, x, rank, fft_length=None, use_placeholder=False,
                       rtol=1e-4, atol=1e-4):
    x_np = self._np_fft(x, rank, fft_length)
    if use_placeholder:
      x_ph = array_ops.placeholder(dtype=dtypes.as_dtype(x.dtype))
      x_tf = self._tf_fft(x_ph, rank, fft_length, feed_dict={x_ph: x})
    else:
      x_tf = self._tf_fft(x, rank, fft_length)

    self.assertAllClose(x_np, x_tf, rtol=rtol, atol=atol)

  def _compare_backward(self, x, rank, fft_length=None, use_placeholder=False,
                        rtol=1e-4, atol=1e-4):
    x_np = self._np_ifft(x, rank, fft_length)
    if use_placeholder:
      x_ph = array_ops.placeholder(dtype=dtypes.as_dtype(x.dtype))
      x_tf = self._tf_ifft(x_ph, rank, fft_length, feed_dict={x_ph: x})
    else:
      x_tf = self._tf_ifft(x, rank, fft_length)

    self.assertAllClose(x_np, x_tf, rtol=rtol, atol=atol)

  def _check_memory_fail(self, x, rank):
    config = config_pb2.ConfigProto()
    config.gpu_options.per_process_gpu_memory_fraction = 1e-2
    with self.cached_session(config=config, force_gpu=True):
      self._tf_fft(x, rank, fft_length=None)

  def _check_grad_complex(self, func, x, y, result_is_complex=True,
                          rtol=1e-2, atol=1e-2):
    with self.cached_session(use_gpu=True):
      def f(inx, iny):
        inx.set_shape(x.shape)
        iny.set_shape(y.shape)
        # func is a forward or inverse, real or complex, batched or unbatched
        # FFT function with a complex input.
        z = func(math_ops.complex(inx, iny))
        # loss = sum(|z|^2)
        loss = math_ops.reduce_sum(math_ops.real(z * math_ops.conj(z)))
        return loss

      ((x_jacob_t, y_jacob_t), (x_jacob_n, y_jacob_n)) = (
          gradient_checker_v2.compute_gradient(f, [x, y], delta=1e-2))

    self.assertAllClose(x_jacob_t, x_jacob_n, rtol=rtol, atol=atol)
    self.assertAllClose(y_jacob_t, y_jacob_n, rtol=rtol, atol=atol)

  def _check_grad_real(self, func, x, rtol=1e-2, atol=1e-2):
    def f(inx):
      inx.set_shape(x.shape)
      # func is a forward RFFT function (batched or unbatched).
      z = func(inx)
      # loss = sum(|z|^2)
      loss = math_ops.reduce_sum(math_ops.real(z * math_ops.conj(z)))
      return loss

    (x_jacob_t,), (x_jacob_n,) = gradient_checker_v2.compute_gradient(
        f, [x], delta=1e-2)
    self.assertAllClose(x_jacob_t, x_jacob_n, rtol=rtol, atol=atol)


@test_util.run_all_in_graph_and_eager_modes
class FFTOpsTest(BaseFFTOpsTest):

  def _tf_fft(self, x, rank, fft_length=None, feed_dict=None):
    # fft_length unused for complex FFTs.
    with self.cached_session(use_gpu=True) as sess:
      return sess.run(self._tf_fft_for_rank(rank)(x), feed_dict=feed_dict)

  def _tf_ifft(self, x, rank, fft_length=None, feed_dict=None):
    # fft_length unused for complex FFTs.
    with self.cached_session(use_gpu=True) as sess:
      return sess.run(self._tf_ifft_for_rank(rank)(x), feed_dict=feed_dict)

  def _np_fft(self, x, rank, fft_length=None):
    if rank == 1:
      return np.fft.fft2(x, s=fft_length, axes=(-1,))
    elif rank == 2:
      return np.fft.fft2(x, s=fft_length, axes=(-2, -1))
    elif rank == 3:
      return np.fft.fft2(x, s=fft_length, axes=(-3, -2, -1))
    else:
      raise ValueError("invalid rank")

  def _np_ifft(self, x, rank, fft_length=None):
    if rank == 1:
      return np.fft.ifft2(x, s=fft_length, axes=(-1,))
    elif rank == 2:
      return np.fft.ifft2(x, s=fft_length, axes=(-2, -1))
    elif rank == 3:
      return np.fft.ifft2(x, s=fft_length, axes=(-3, -2, -1))
    else:
      raise ValueError("invalid rank")

  def _tf_fft_for_rank(self, rank):
    if rank == 1:
      return fft_ops.fft
    elif rank == 2:
      return fft_ops.fft2d
    elif rank == 3:
      return fft_ops.fft3d
    else:
      raise ValueError("invalid rank")

  def _tf_ifft_for_rank(self, rank):
    if rank == 1:
      return fft_ops.ifft
    elif rank == 2:
      return fft_ops.ifft2d
    elif rank == 3:
      return fft_ops.ifft3d
    else:
      raise ValueError("invalid rank")

  def test_empty(self):
    for np_type in (np.complex64, np.complex128):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 3):
          x = np.zeros((0,) * dims).astype(np_type)
          self.assertEqual(x.shape, self._tf_fft(x, rank).shape)
          self.assertEqual(x.shape, self._tf_ifft(x, rank).shape)

  def test_basic(self):
    for np_type, tol in ((np.complex64, 1e-4), (np.complex128, 1e-8)):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 3):
          self._compare(
              np.mod(np.arange(np.power(4, dims)), 10).reshape(
                  (4,) * dims).astype(np_type), rank, rtol=tol, atol=tol)

  def test_large_batch(self):
    if test.is_gpu_available(cuda_only=True):
      rank = 1
      for dims in xrange(rank, rank + 3):
        for np_type, tol in ((np.complex64, 1e-4), (np.complex128, 1e-5)):
          self._compare(
              np.mod(np.arange(np.power(128, dims)), 10).reshape(
                  (128,) * dims).astype(np_type), rank, rtol=tol, atol=tol)

  # TODO(yangzihao): Disable before we can figure out a way to
  # properly test memory fail for large batch fft.
  # def test_large_batch_memory_fail(self):
  #   if test.is_gpu_available(cuda_only=True):
  #     rank = 1
  #     for dims in xrange(rank, rank + 3):
  #       self._check_memory_fail(
  #           np.mod(np.arange(np.power(128, dims)), 64).reshape(
  #               (128,) * dims).astype(np.complex64), rank)

  def test_placeholder(self):
    if context.executing_eagerly():
      return
    for np_type, tol in ((np.complex64, 1e-4), (np.complex128, 1e-8)):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 3):
          self._compare(
              np.mod(np.arange(np.power(4, dims)), 10).reshape(
                  (4,) * dims).astype(np_type),
              rank, use_placeholder=True, rtol=tol, atol=tol)

  def test_random(self):
    for np_type, tol in ((np.complex64, 1e-4), (np.complex128, 5e-6)):
      def gen(shape):
        n = np.prod(shape)
        re = np.random.uniform(size=n)
        im = np.random.uniform(size=n)
        return (re + im * 1j).reshape(shape)

      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 3):
          self._compare(gen((4,) * dims).astype(np_type), rank,
                        rtol=tol, atol=tol)

  def test_random_1d(self):
    for np_type in (np.complex64, np.complex128):
      has_gpu = test.is_gpu_available(cuda_only=True)
      tol = {(np.complex64, True): 1e-4,
             (np.complex64, False): 1e-2,
             (np.complex128, True): 1e-4,
             (np.complex128, False): 1e-2}[(np_type, has_gpu)]
      def gen(shape):
        n = np.prod(shape)
        re = np.random.uniform(size=n)
        im = np.random.uniform(size=n)
        return (re + im * 1j).reshape(shape)

      # Check a variety of power-of-2 FFT sizes.
      for dim in (128, 256, 512, 1024):
        self._compare(gen((dim,)).astype(np_type), 1, rtol=tol, atol=tol)

      # Check a variety of non-power-of-2 FFT sizes.
      for dim in (127, 255, 511, 1023):
        self._compare(gen((dim,)).astype(np_type), 1, rtol=tol, atol=tol)

  def test_error(self):
    # TODO(rjryan): Fix this test under Eager.
    if context.executing_eagerly():
      return
    for rank in VALID_FFT_RANKS:
      for dims in xrange(0, rank):
        x = np.zeros((1,) * dims).astype(np.complex64)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Shape must be .*rank {}.*".format(rank)):
          self._tf_fft(x, rank)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Shape must be .*rank {}.*".format(rank)):
          self._tf_ifft(x, rank)

  def test_grad_simple(self):
    for np_type, tol in ((np.float32, 1e-4), (np.float64, 1e-10)):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 2):
          re = np.ones(shape=(4,) * dims, dtype=np_type) / 10.0
          im = np.zeros(shape=(4,) * dims, dtype=np_type)
          self._check_grad_complex(self._tf_fft_for_rank(rank), re, im,
                                   rtol=tol, atol=tol)
          self._check_grad_complex(self._tf_ifft_for_rank(rank), re, im,
                                   rtol=tol, atol=tol)

  def test_grad_random(self):
    for np_type, tol in ((np.float32, 1e-2), (np.float64, 1e-10)):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 2):
          re = np.random.rand(*((3,) * dims)).astype(np_type) * 2 - 1
          im = np.random.rand(*((3,) * dims)).astype(np_type) * 2 - 1
          self._check_grad_complex(self._tf_fft_for_rank(rank), re, im,
                                   rtol=tol, atol=tol)
          self._check_grad_complex(self._tf_ifft_for_rank(rank), re, im,
                                   rtol=tol, atol=tol)


@test_util.run_all_in_graph_and_eager_modes
class RFFTOpsTest(BaseFFTOpsTest):

  def _compare_backward(self, x, rank, fft_length=None, use_placeholder=False):
    super(RFFTOpsTest, self)._compare_backward(x, rank, fft_length,
                                               use_placeholder)

  def _tf_fft(self, x, rank, fft_length=None, feed_dict=None):
    with self.cached_session(use_gpu=True) as sess:
      return sess.run(
          self._tf_fft_for_rank(rank)(x, fft_length), feed_dict=feed_dict)

  def _tf_ifft(self, x, rank, fft_length=None, feed_dict=None):
    with self.cached_session(use_gpu=True) as sess:
      return sess.run(
          self._tf_ifft_for_rank(rank)(x, fft_length), feed_dict=feed_dict)

  def _np_fft(self, x, rank, fft_length=None):
    if rank == 1:
      return np.fft.rfft2(x, s=fft_length, axes=(-1,))
    elif rank == 2:
      return np.fft.rfft2(x, s=fft_length, axes=(-2, -1))
    elif rank == 3:
      return np.fft.rfft2(x, s=fft_length, axes=(-3, -2, -1))
    else:
      raise ValueError("invalid rank")

  def _np_ifft(self, x, rank, fft_length=None):
    if rank == 1:
      return np.fft.irfft2(x, s=fft_length, axes=(-1,))
    elif rank == 2:
      return np.fft.irfft2(x, s=fft_length, axes=(-2, -1))
    elif rank == 3:
      return np.fft.irfft2(x, s=fft_length, axes=(-3, -2, -1))
    else:
      raise ValueError("invalid rank")

  def _tf_fft_for_rank(self, rank):
    if rank == 1:
      return fft_ops.rfft
    elif rank == 2:
      return fft_ops.rfft2d
    elif rank == 3:
      return fft_ops.rfft3d
    else:
      raise ValueError("invalid rank")

  def _tf_ifft_for_rank(self, rank):
    if rank == 1:
      return fft_ops.irfft
    elif rank == 2:
      return fft_ops.irfft2d
    elif rank == 3:
      return fft_ops.irfft3d
    else:
      raise ValueError("invalid rank")

  def test_empty(self):
    for rank in VALID_FFT_RANKS:
      for dims in xrange(rank, rank + 3):
        x = np.zeros((0,) * dims).astype(np.float32)
        self.assertEqual(x.shape, self._tf_fft(x, rank).shape)
        x = np.zeros((0,) * dims).astype(np.complex64)
        self.assertEqual(x.shape, self._tf_ifft(x, rank).shape)

  def test_basic(self):
    for rank in VALID_FFT_RANKS:
      for dims in xrange(rank, rank + 3):
        for size in (5, 6):
          inner_dim = size // 2 + 1
          r2c = np.mod(np.arange(np.power(size, dims)), 10).reshape(
              (size,) * dims)
          self._compare_forward(r2c.astype(np.float32), rank, (size,) * rank)
          c2r = np.mod(np.arange(np.power(size, dims - 1) * inner_dim),
                       10).reshape((size,) * (dims - 1) + (inner_dim,))
          self._compare_backward(
              c2r.astype(np.complex64), rank, (size,) * rank)

  def test_large_batch(self):
    if test.is_gpu_available(cuda_only=True):
      rank = 1
      for dims in xrange(rank, rank + 3):
        for size in (64, 128):
          inner_dim = size // 2 + 1
          r2c = np.mod(np.arange(np.power(size, dims)), 10).reshape(
              (size,) * dims)
          self._compare_forward(r2c.astype(np.float32), rank, (size,) * rank)
          c2r = np.mod(np.arange(np.power(size, dims - 1) * inner_dim),
                       10).reshape((size,) * (dims - 1) + (inner_dim,))
          self._compare_backward(c2r.astype(np.complex64), rank, (size,) * rank)

  def test_placeholder(self):
    if context.executing_eagerly():
      return
    for rank in VALID_FFT_RANKS:
      for dims in xrange(rank, rank + 3):
        for size in (5, 6):
          inner_dim = size // 2 + 1
          r2c = np.mod(np.arange(np.power(size, dims)), 10).reshape(
              (size,) * dims)
          self._compare_forward(
              r2c.astype(np.float32),
              rank, (size,) * rank,
              use_placeholder=True)
          c2r = np.mod(np.arange(np.power(size, dims - 1) * inner_dim),
                       10).reshape((size,) * (dims - 1) + (inner_dim,))
          self._compare_backward(
              c2r.astype(np.complex64),
              rank, (size,) * rank,
              use_placeholder=True)

  def test_fft_lenth(self):
    if test.is_gpu_available(cuda_only=True):
      for rank in VALID_FFT_RANKS:
        for dims in xrange(rank, rank + 3):
          for size in (5, 6):
            inner_dim = size // 2 + 1
            r2c = np.mod(np.arange(np.power(size, dims)), 10).reshape(
                (size,) * dims)
            c2r = np.mod(np.arange(np.power(size, dims - 1) * inner_dim),
                         10).reshape((size,) * (dims - 1) + (inner_dim,))
            # Test truncation (FFT size < dimensions).
            fft_length = (size - 2,) * rank
            self._compare_forward(r2c.astype(np.float32), rank, fft_length)
            self._compare_backward(c2r.astype(np.complex64), rank, fft_length)
            # Confirm it works with unknown shapes as well.
            if not context.executing_eagerly():
              self._compare_forward(
                  r2c.astype(np.float32),
                  rank,
                  fft_length,
                  use_placeholder=True)
              self._compare_backward(
                  c2r.astype(np.complex64),
                  rank,
                  fft_length,
                  use_placeholder=True)
            # Test padding (FFT size > dimensions).
            fft_length = (size + 2,) * rank
            self._compare_forward(r2c.astype(np.float32), rank, fft_length)
            self._compare_backward(c2r.astype(np.complex64), rank, fft_length)
            # Confirm it works with unknown shapes as well.
            if not context.executing_eagerly():
              self._compare_forward(
                  r2c.astype(np.float32),
                  rank,
                  fft_length,
                  use_placeholder=True)
              self._compare_backward(
                  c2r.astype(np.complex64),
                  rank,
                  fft_length,
                  use_placeholder=True)

  def test_random(self):
    def gen_real(shape):
      n = np.prod(shape)
      re = np.random.uniform(size=n)
      ret = re.reshape(shape)
      return ret

    def gen_complex(shape):
      n = np.prod(shape)
      re = np.random.uniform(size=n)
      im = np.random.uniform(size=n)
      ret = (re + im * 1j).reshape(shape)
      return ret

    for rank in VALID_FFT_RANKS:
      for dims in xrange(rank, rank + 3):
        for size in (5, 6):
          inner_dim = size // 2 + 1
          self._compare_forward(gen_real((size,) * dims), rank, (size,) * rank)
          complex_dims = (size,) * (dims - 1) + (inner_dim,)
          self._compare_backward(
              gen_complex(complex_dims), rank, (size,) * rank)

  def test_error(self):
    # TODO(rjryan): Fix this test under Eager.
    if context.executing_eagerly():
      return
    for rank in VALID_FFT_RANKS:
      for dims in xrange(0, rank):
        x = np.zeros((1,) * dims).astype(np.complex64)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Shape .* must have rank at least {}".format(rank)):
          self._tf_fft(x, rank)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Shape .* must have rank at least {}".format(rank)):
          self._tf_ifft(x, rank)
      for dims in xrange(rank, rank + 2):
        x = np.zeros((1,) * rank)

        # Test non-rank-1 fft_length produces an error.
        fft_length = np.zeros((1, 1)).astype(np.int32)
        with self.assertRaisesWithPredicateMatch(ValueError,
                                                 "Shape .* must have rank 1"):
          self._tf_fft(x, rank, fft_length)
        with self.assertRaisesWithPredicateMatch(ValueError,
                                                 "Shape .* must have rank 1"):
          self._tf_ifft(x, rank, fft_length)

        # Test wrong fft_length length.
        fft_length = np.zeros((rank + 1,)).astype(np.int32)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Dimension must be .*but is {}.*".format(rank + 1)):
          self._tf_fft(x, rank, fft_length)
        with self.assertRaisesWithPredicateMatch(
            ValueError, "Dimension must be .*but is {}.*".format(rank + 1)):
          self._tf_ifft(x, rank, fft_length)

      # Test that calling the kernel directly without padding to fft_length
      # produces an error.
      rffts_for_rank = {
          1: [gen_spectral_ops.rfft, gen_spectral_ops.irfft],
          2: [gen_spectral_ops.rfft2d, gen_spectral_ops.irfft2d],
          3: [gen_spectral_ops.rfft3d, gen_spectral_ops.irfft3d]
      }
      rfft_fn, irfft_fn = rffts_for_rank[rank]
      with self.assertRaisesWithPredicateMatch(
          errors.InvalidArgumentError,
          "Input dimension .* must have length of at least 6 but got: 5"):
        x = np.zeros((5,) * rank).astype(np.float32)
        fft_length = [6] * rank
        with self.cached_session():
          self.evaluate(rfft_fn(x, fft_length))

      with self.assertRaisesWithPredicateMatch(
          errors.InvalidArgumentError,
          "Input dimension .* must have length of at least .* but got: 3"):
        x = np.zeros((3,) * rank).astype(np.complex64)
        fft_length = [6] * rank
        with self.cached_session():
          self.evaluate(irfft_fn(x, fft_length))

  def test_grad_simple(self):
    for rank in VALID_FFT_RANKS:
      # rfft3d/irfft3d do not have gradients yet.
      if rank == 3:
        continue
      for dims in xrange(rank, rank + 2):
        for size in (5, 6):
          re = np.ones(shape=(size,) * dims, dtype=np.float32)
          im = -np.ones(shape=(size,) * dims, dtype=np.float32)
          self._check_grad_real(self._tf_fft_for_rank(rank), re)
          self._check_grad_complex(
              self._tf_ifft_for_rank(rank), re, im, result_is_complex=False)

  def test_grad_random(self):
    for rank in VALID_FFT_RANKS:
      # rfft3d/irfft3d do not have gradients yet.
      if rank == 3:
        continue
      for dims in xrange(rank, rank + 2):
        for size in (5, 6):
          re = np.random.rand(*((size,) * dims)).astype(np.float32) * 2 - 1
          im = np.random.rand(*((size,) * dims)).astype(np.float32) * 2 - 1
          self._check_grad_real(self._tf_fft_for_rank(rank), re)
          self._check_grad_complex(
              self._tf_ifft_for_rank(rank), re, im, result_is_complex=False)


@test_util.run_all_in_graph_and_eager_modes
class FFTShiftTest(test.TestCase):

  def test_definition(self):
    with self.session():
      x = [0, 1, 2, 3, 4, -4, -3, -2, -1]
      y = [-4, -3, -2, -1, 0, 1, 2, 3, 4]
      self.assertAllEqual(fft_ops.fftshift(x), y)
      self.assertAllEqual(fft_ops.ifftshift(y), x)
      x = [0, 1, 2, 3, 4, -5, -4, -3, -2, -1]
      y = [-5, -4, -3, -2, -1, 0, 1, 2, 3, 4]
      self.assertAllEqual(fft_ops.fftshift(x), y)
      self.assertAllEqual(fft_ops.ifftshift(y), x)

  def test_axes_keyword(self):
    with self.session():
      freqs = [[0, 1, 2], [3, 4, -4], [-3, -2, -1]]
      shifted = [[-1, -3, -2], [2, 0, 1], [-4, 3, 4]]
      self.assertAllEqual(fft_ops.fftshift(freqs, axes=(0, 1)), shifted)
      self.assertAllEqual(
          fft_ops.fftshift(freqs, axes=0),
          fft_ops.fftshift(freqs, axes=(0,)))
      self.assertAllEqual(fft_ops.ifftshift(shifted, axes=(0, 1)), freqs)
      self.assertAllEqual(
          fft_ops.ifftshift(shifted, axes=0),
          fft_ops.ifftshift(shifted, axes=(0,)))
      self.assertAllEqual(fft_ops.fftshift(freqs), shifted)
      self.assertAllEqual(fft_ops.ifftshift(shifted), freqs)

  def test_numpy_compatibility(self):
    with self.session():
      x = [0, 1, 2, 3, 4, -4, -3, -2, -1]
      y = [-4, -3, -2, -1, 0, 1, 2, 3, 4]
      self.assertAllEqual(fft_ops.fftshift(x), np.fft.fftshift(x))
      self.assertAllEqual(fft_ops.ifftshift(y), np.fft.ifftshift(y))
      x = [0, 1, 2, 3, 4, -5, -4, -3, -2, -1]
      y = [-5, -4, -3, -2, -1, 0, 1, 2, 3, 4]
      self.assertAllEqual(fft_ops.fftshift(x), np.fft.fftshift(x))
      self.assertAllEqual(fft_ops.ifftshift(y), np.fft.ifftshift(y))
      freqs = [[0, 1, 2], [3, 4, -4], [-3, -2, -1]]
      shifted = [[-1, -3, -2], [2, 0, 1], [-4, 3, 4]]
      self.assertAllEqual(
          fft_ops.fftshift(freqs, axes=(0, 1)),
          np.fft.fftshift(freqs, axes=(0, 1)))
      self.assertAllEqual(
          fft_ops.ifftshift(shifted, axes=(0, 1)),
          np.fft.ifftshift(shifted, axes=(0, 1)))

  def test_placeholder(self):
    if context.executing_eagerly():
      return
    x = array_ops.placeholder(shape=[None, None, None], dtype="float32")
    axes_to_test = [None, 1, [1, 2]]
    for axes in axes_to_test:
      y_fftshift = fft_ops.fftshift(x, axes=axes)
      y_ifftshift = fft_ops.ifftshift(x, axes=axes)
      with self.session() as sess:
        x_np = np.random.rand(16, 256, 256)
        y_fftshift_res, y_ifftshift_res = sess.run(
            [y_fftshift, y_ifftshift],
            feed_dict={x: x_np},
        )
        self.assertAllClose(y_fftshift_res, np.fft.fftshift(x_np, axes=axes))
        self.assertAllClose(y_ifftshift_res, np.fft.ifftshift(x_np, axes=axes))


if __name__ == "__main__":
  test.main()
