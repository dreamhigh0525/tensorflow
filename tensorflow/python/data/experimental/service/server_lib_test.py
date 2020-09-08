# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tf.data service server lib."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tempfile
from tensorflow.python.data.experimental.service import server_lib
from tensorflow.python.platform import test


class ServerLibTest(test.TestCase):

  def testStartDispatcher(self):
    dispatcher = server_lib.DispatchServer(start=False)
    dispatcher.start()

  def testStartDispatcherWithPortConfig(self):
    config = server_lib.DispatcherConfig(port=5000)
    dispatcher = server_lib.DispatchServer(config=config, start=False)
    dispatcher.start()
    assert dispatcher.target == "grpc://localhost:5000"

  def testStartDispatcherWithWorkDirConfig(self):
    temp_dir = tempfile.mkdtemp()
    config = server_lib.DispatcherConfig(work_dir=temp_dir)
    dispatcher = server_lib.DispatchServer(config=config, start=False)
    dispatcher.start()

  def testStartDispatcherWithFaultTolerantConfig(self):
    temp_dir = tempfile.mkdtemp()
    config = server_lib.DispatcherConfig(work_dir=temp_dir,
                                         fault_tolerant_mode=True)
    dispatcher = server_lib.DispatchServer(config=config, start=False)
    dispatcher.start()

  def testStartDispatcherWithWrongFaultTolerantConfig(self):
    config = server_lib.DispatcherConfig(fault_tolerant_mode=True)
    error = "Cannot enable fault tolerant mode without configuring a work_dir"
    with self.assertRaisesRegex(ValueError, error):
      dispatcher = server_lib.DispatchServer(config=config, start=False)
      dispatcher.start()

  def testMultipleStartDispatcher(self):
    dispatcher = server_lib.DispatchServer(start=True)
    dispatcher.start()

  def testStartWorker(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address), start=False)
    worker.start()

  def testStartWorkerWithPortConfig(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address, port=5005), start=False)
    worker.start()
    assert worker._address == "localhost:5005"

  def testMultipleStartWorker(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address), start=True)
    worker.start()

  def testStopDispatcher(self):
    dispatcher = server_lib.DispatchServer()
    dispatcher._stop()
    dispatcher._stop()

  def testStopWorker(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address))
    worker._stop()
    worker._stop()

  def testStopStartDispatcher(self):
    dispatcher = server_lib.DispatchServer()
    dispatcher._stop()
    with self.assertRaisesRegex(
        RuntimeError, "Server cannot be started after it has been stopped"):
      dispatcher.start()

  def testStopStartWorker(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address))
    worker._stop()
    with self.assertRaisesRegex(
        RuntimeError, "Server cannot be started after it has been stopped"):
      worker.start()

  def testJoinDispatcher(self):
    dispatcher = server_lib.DispatchServer()
    dispatcher._stop()
    dispatcher.join()

  def testJoinWorker(self):
    dispatcher = server_lib.DispatchServer()
    worker = server_lib.WorkerServer(
        server_lib.WorkerConfig(dispatcher._address))
    worker._stop()
    worker.join()

  def testDispatcherNumWorkers(self):
    dispatcher = server_lib.DispatchServer()
    self.assertEqual(0, dispatcher._num_workers())
    worker1 = server_lib.WorkerServer(  # pylint: disable=unused-variable
        server_lib.WorkerConfig(dispatcher._address))
    self.assertEqual(1, dispatcher._num_workers())
    worker2 = server_lib.WorkerServer(  # pylint: disable=unused-variable
        server_lib.WorkerConfig(dispatcher._address))
    self.assertEqual(2, dispatcher._num_workers())


if __name__ == "__main__":
  test.main()
