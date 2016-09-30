# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
"""Framework of debug-wrapped sessions."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import shutil
import tempfile

import numpy as np

from tensorflow.python.client import session
from tensorflow.python.debug import debug_data
from tensorflow.python.debug import framework
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import test_util
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import googletest


class TestDebugWrapperSession(framework.BaseDebugWrapperSession):
  """A concrete implementation of BaseDebugWrapperSession for test."""

  def __init__(self, sess, dump_root, observer):
    # Supply dump root.
    self._dump_root = dump_root

    # Supply observer.
    self._obs = observer

    # Invoke superclass constructor.
    framework.BaseDebugWrapperSession.__init__(self, sess)

  def on_session_init(self, request):
    """Override abstract on-session-init callback method."""

    self._obs["sess_init_count"] += 1
    self._obs["request_sess"] = request.session

    return framework.OnSessionInitResponse(
        framework.OnSessionInitAction.PROCEED)

  def on_run_start(self, request):
    """Override abstract on-run-start callback method."""

    self._obs["on_run_start_count"] += 1
    self._obs["run_fetches"] = request.fetches
    self._obs["run_feed_dict"] = request.feed_dict

    return framework.OnRunStartResponse(
        framework.OnRunStartAction.DEBUG_RUN,
        ["file://" + self._dump_root])

  def on_run_end(self, request):
    """Override abstract on-run-end callback method."""

    self._obs["on_run_end_count"] += 1
    self._obs["performed_action"] = request.performed_action

    return framework.OnRunEndResponse()


class TestDebugWrapperSessionBadAction(framework.BaseDebugWrapperSession):
  """A concrete implementation of BaseDebugWrapperSession for test.

  This class intentionally puts a bad action value in OnSessionInitResponse
  and/or in OnRunStartAction to test the handling of such invalid cases.
  """

  def __init__(
      self,
      sess,
      bad_init_action=None,
      bad_run_start_action=None,
      bad_debug_urls=None):
    """Constructor.

    Args:
      sess: The TensorFlow Session object to be wrapped.
      bad_init_action: (str) bad action value to be returned during the
        on-session-init callback.
      bad_run_start_action: (str) bad action value to be returned during the
        the on-run-start callback.
      bad_debug_urls: Bad URL values to be returned during the on-run-start
        callback.
    """

    self._bad_init_action = bad_init_action
    self._bad_run_start_action = bad_run_start_action
    self._bad_debug_urls = bad_debug_urls

    # Invoke superclass constructor.
    framework.BaseDebugWrapperSession.__init__(self, sess)

  def on_session_init(self, request):
    if self._bad_init_action:
      return framework.OnSessionInitResponse(self._bad_init_action)
    else:
      return framework.OnSessionInitResponse(
          framework.OnSessionInitAction.PROCEED)

  def on_run_start(self, request):
    debug_urls = self._bad_debug_urls or []

    if self._bad_run_start_action:
      return framework.OnRunStartResponse(
          self._bad_run_start_action, debug_urls)
    else:
      return framework.OnRunStartResponse(
          framework.OnRunStartAction.DEBUG_RUN, debug_urls)

  def on_run_end(self, request):
    return framework.OnRunEndResponse()


class DebugWrapperSessionTest(test_util.TensorFlowTestCase):

  def setUp(self):
    self._observer = {
        "sess_init_count": 0,
        "request_sess": None,
        "on_run_start_count": 0,
        "run_fetches": None,
        "run_feed_dict": None,
        "on_run_end_count": 0,
        "performed_action": None,
    }

    self._dump_root = tempfile.mkdtemp()

    self._sess = session.Session()

    self._a_init_val = np.array([[5.0, 3.0], [-1.0, 0.0]])
    self._b_init_val = np.array([[2.0], [-1.0]])
    self._c_val = np.array([[-4.0], [6.0]])

    self._a_init = constant_op.constant(
        self._a_init_val, shape=[2, 2], name="a1_init")
    self._b_init = constant_op.constant(
        self._b_init_val, shape=[2, 1], name="b_init")

    self._a = variables.Variable(self._a_init, name="a1")
    self._b = variables.Variable(self._b_init, name="b")
    self._c = constant_op.constant(self._c_val, shape=[2, 1], name="c")

    # Matrix product of a and b.
    self._p = math_ops.matmul(self._a, self._b, name="p1")

    # Sum of two vectors.
    self._s = math_ops.add(self._p, self._c, name="s")

    # Initialize the variables.
    self._sess.run(self._a.initializer)
    self._sess.run(self._b.initializer)

  def tearDown(self):
    # Tear down temporary dump directory.
    shutil.rmtree(self._dump_root)

  def testSessionInit(self):
    self.assertEqual(0, self._observer["sess_init_count"])

    TestDebugWrapperSession(self._sess, self._dump_root, self._observer)

    # Assert that on-session-init callback is invoked.
    self.assertEqual(1, self._observer["sess_init_count"])

    # Assert that the request to the on-session-init callback carries the
    # correct session object.
    self.assertEqual(self._sess, self._observer["request_sess"])

  def testSessionRun(self):
    wrapper = TestDebugWrapperSession(
        self._sess, self._dump_root, self._observer)

    # Check initial state of the observer.
    self.assertEqual(0, self._observer["on_run_start_count"])
    self.assertEqual(0, self._observer["on_run_end_count"])

    s = wrapper.run(self._s)

    # Assert the run return value is correct.
    self.assertAllClose(np.array([[3.0], [4.0]]), s)

    # Assert the on-run-start method is invoked.
    self.assertEqual(1, self._observer["on_run_start_count"])

    # Assert the on-run-start request reflects the correct fetch.
    self.assertEqual(self._s, self._observer["run_fetches"])

    # Assert the on-run-start request reflects the correct feed_dict.
    self.assertIsNone(self._observer["run_feed_dict"])

    # Assert the file debug URL has led to dump on the filesystem.
    dump = debug_data.DebugDumpDir(self._dump_root)
    self.assertEqual(7, len(dump.dumped_tensor_data))

    # Assert the on-run-end method is invoked.
    self.assertEqual(1, self._observer["on_run_end_count"])

    # Assert the performed action field in the on-run-end callback request is
    # correct.
    self.assertEqual(
        framework.OnRunStartAction.DEBUG_RUN,
        self._observer["performed_action"])

  def testSessionInitInvalidSessionType(self):
    """Attempt to wrap a non-Session-type object should cause an exception."""

    wrapper = TestDebugWrapperSessionBadAction(self._sess)
    with self.assertRaisesRegexp(TypeError, "Expected type .*; got type .*"):
      TestDebugWrapperSessionBadAction(wrapper)

  def testSessionInitBadActionValue(self):
    with self.assertRaisesRegexp(
        ValueError, "Invalid OnSessionInitAction value: nonsense_action"):
      TestDebugWrapperSessionBadAction(
          self._sess, bad_init_action="nonsense_action")

  def testRunStartBadActionValue(self):
    wrapper = TestDebugWrapperSessionBadAction(
        self._sess, bad_run_start_action="nonsense_action")

    with self.assertRaisesRegexp(
        ValueError, "Invalid OnRunStartAction value: nonsense_action"):
      wrapper.run(self._s)

  def testRunStartBadURLs(self):
    # debug_urls ought to be a list of str, not a str. So an exception should
    # be raised during a run() call.
    wrapper = TestDebugWrapperSessionBadAction(
        self._sess, bad_debug_urls="file://foo")

    with self.assertRaisesRegexp(TypeError, "Expected type .*; got type .*"):
      wrapper.run(self._s)


if __name__ == "__main__":
  googletest.main()
