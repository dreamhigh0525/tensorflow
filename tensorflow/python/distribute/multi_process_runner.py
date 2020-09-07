# Lint as: python3
# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
"""Multi-process runner for testing purpose."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import contextlib
import json
import os
import signal
import sys
import threading
import time
import unittest

from absl import logging
import six
from six.moves import queue as Queue

from tensorflow.python import tf2
from tensorflow.python.compat import v2_compat
from tensorflow.python.distribute import multi_process_lib
from tensorflow.python.eager import context

multiprocessing = multi_process_lib.multiprocessing

# pylint: disable=g-import-not-at-top
try:
  # `faulthandler` is not available in py2.
  import faulthandler
except ImportError:
  faulthandler = None

# TODO(b/150264776): Remove after resolving CI issue.
try:
  import dill
except ImportError:
  dill = None

# TODO(b/150264776): Remove after resolving CI issue.
try:
  import tblib.pickling_support
  # For pickling traceback objects.
  tblib.pickling_support.install()
except ImportError:
  pass


# _ProcessStatusInfo contains process status information. When is_successful
# attribute is True, the subprocess has ended successfully, or if False, the
# exception stack trace info is stored in exc_info to pass on to parent process
# to be re-raised.
_ProcessStatusInfo = collections.namedtuple(
    '_ProcessStatusInfo',
    ['task_type', 'task_id', 'is_successful', 'exc_info', 'return_value'])

# Information returned from a successful MultiProcessRunner run.
MultiProcessRunnerResult = collections.namedtuple('MultiProcessRunnerResult',
                                                  ['return_value', 'stdout'])

TestEnvironment = collections.namedtuple('TestEnvironment', [
    'task_type', 'task_id', 'cluster_spec', 'rpc_layer', 'grpc_fail_fast',
    'v2_enabled', 'executing_eagerly'
])

# Resources for communication between worker processes and the main process.
#
# `process_status_queue` is used by `multi_process_runner` internally for
#   communication from subprocesses to the parent process for whether it's been
#   successful, and if not what the error stack trace is.
# `parent_to_sub_queue` is used for communications from parent to subprocess.
#   Currently this is only used to terminate subprocesses.
# TODO(rchao): Remove this once subprocess is terminated by SIGKILL.
# `streaming_pipe_w` is to stream stdout and stderr from subprocesses to parent
#   process.
# `barrier` is a barrier for the party of all subprocesses.
Resources = collections.namedtuple('Resources', [
    'process_status_queue', 'parent_to_sub_queue', 'streaming_pipe_w', 'barrier'
])

# Default time out sec is selected so that it's handled before the default
# "medium" timeout of the test runs.
_DEFAULT_TIMEOUT_SEC = 200

# The timeout in seconds to wait to force kill a child process. When a child
# process times out we first try to SIGTERM it so that it has a chance to dump
# stacktraces. However dumping stacktrace can take a long time.
_FORCE_KILL_WAIT_SEC = 30


class MultiProcessRunner(object):
  """A utility class to start multiple processes to simulate a cluster.

  We need to use multiple processes to simulate a cluster in TF 2.0 tests
  because TF 2.0 has some process-global data structures that have to be
  separated by processes. We also need child processes to test out our fault
  tolerance because shutting down a standard TensorFlow server within its
  process is not supported.

  Note: the main test program that uses this runner class must run main program
  via `test_main` defined in this file. Using this runner in non-test binaries
  is not supported yet.

  This class is not thread-safe. Child processes will inherit TF2 behavior flag.
  """

  def __init__(self,
               proc_func,
               cluster_spec,
               rpc_layer=None,
               max_run_time=None,
               grpc_fail_fast=None,
               stream_stdout=True,
               list_stdout=False,
               use_dill_for_args=True,
               daemon=False,
               dependence_on_chief=True,
               auto_restart=False,
               args=None,
               kwargs=None):
    """Creates a multi-process runner.

    Args:
      proc_func: Function to be run on child processes. This will be run on
        processes for all task types.
      cluster_spec: Dict for cluster spec. The following is an example of
        cluster with three workers and two ps's.
        {"worker": ["worker0.example.com:2222",
                    "worker1.example.com:2222",
                    "worker2.example.com:2222"],
         "ps": ["ps0.example.com:2222",
                "ps1.example.com:2222"]}
      rpc_layer: RPC layer to use. Default value is 'grpc'.
      max_run_time: If set, child processes is forced to exit at approximately
        this many seconds after `start` is called. We achieve this through
        `signal.alarm()` api. Note that this is best effort at Python level
        since Python signal handler does not get executed when it runs lower
        level C/C++ code. So it can be delayed for arbitrarily long time.
        If any of the child process is still running when `max_run_time` is up,
        they will be force-terminated and a `UnexpectedSubprocessExitError`
        may be raised at `join()`.
      grpc_fail_fast: Whether GRPC connection between processes should fail
        without retrying. Defaults to None, in which case the environment
        variable is not explicitly set.
      stream_stdout: True if the output/error from the subprocesses should be
        streamed to be printed in parent process' log. Defaults to True.
      list_stdout: True if the output/error from the subprocesses should be
        collected to be attached to the resulting `MultiProcessRunnerResult`
        returned from `MultiProcessRunner.join()`. If True, the list of stdout
        can be retrieved via `MultiProcessRunnerResult.stdout` attribute.
        Defaults to False.
      use_dill_for_args: Whether to use dill to pickle `args` and `kwargs`. dill
        can pickle more objects, but doesn't work with types in
        `multiprocessing` library like `Mutex`.
      daemon: Whether to start processes as daemons.
      dependence_on_chief: Whether to terminates the cluster if the chief exits.
        If auto_restart is True, it only terminates the cluster if the chief
        exits with a zero exit code.
      auto_restart: Whether to automatically restart processes that exit with
        non-zero exit code.
      args: Positional arguments to be sent to functions run on processes.
      kwargs: Keyword arguments to be sent to functions run on processes.

    Raises:
      RuntimeError: if `multi_process_runner.test_main()` is not called.
      ValueError: if there are more than one chief in the `cluster_spec`.
    """
    assert cluster_spec is not None
    if 'chief' in cluster_spec and len(cluster_spec['chief']) > 1:
      raise ValueError('If chief exists in the cluster, there must be at most '
                       'one chief. Current `cluster_spec` has {} chiefs.'
                       .format(len(cluster_spec['chief'])))
    if not multi_process_lib.initialized():
      raise MultiProcessRunnerNotInitializedError(
          '`multi_process_runner` is not initialized. '
          'Please call `multi_process_runner.test_main()` '
          'within `if __name__ == \'__main__\':` block '
          'in your python module to properly initialize '
          '`multi_process_runner`.')
    if not callable(proc_func):
      raise ValueError('proc_func is not a callable')

    self._proc_func = proc_func
    self._cluster_spec = cluster_spec
    self._rpc_layer = rpc_layer or 'grpc'
    self._max_run_time = max_run_time
    self._grpc_fail_fast = grpc_fail_fast
    self._stream_stdout = stream_stdout
    # TODO(rchao): Revisit list_stdout argument to consider other solution.
    self._list_stdout = list_stdout
    self._dependence_on_chief = dependence_on_chief
    self._use_dill_for_args = use_dill_for_args
    self._daemon = daemon
    self._auto_restart = auto_restart
    self._args = args or ()
    self._kwargs = kwargs or {}

    # Child processes should have the same v2 and eager behavior.
    self._v2_enabled = tf2.enabled()
    self._executing_eagerly = context.executing_eagerly()

    self._joined = False
    self._process_lock = threading.Lock()
    # Guarded by self._process_lock.
    self._processes = {}
    # Record which processes are terminated. Due to a bug in Python<3.7,
    # terminated processes return 255 exit code, which should cause an exception
    # in join().
    # https://bugs.python.org/issue30589
    # Guarded by self._process_lock.
    self._terminated = set()
    self._reading_threads = []

    self._manager = manager()
    self._process_status_queue = self._manager.Queue()
    self._parent_to_sub_queue = self._manager.Queue()
    parties = sum(len(addresses) for addresses in self._cluster_spec.values())
    self._barrier = self._manager.Barrier(parties)

    # We use a queue to collect outputs from worker processes since it's thread
    # safe.
    self._streaming_queue = self._manager.Queue()

    self._watchdog_thread = None

  def set_args(self, args=None, kwargs=None):
    self._args = args or self._args
    self._kwargs = kwargs or self._kwargs

  def _continuously_readline_from_sub(self, pipe_r, task_type, task_id):
    """Function to continuously read lines from subprocesses."""
    with os.fdopen(pipe_r.fileno(), 'r', closefd=False) as reader:
      for line in reader:
        task_string = '[{}-{}]:'.format(task_type, task_id)
        formatted_line = '{} {}'.format(task_string.ljust(14), line)
        if self._stream_stdout:
          # TODO(rchao): Use a lock here to ensure the printed lines are not
          # broken.
          print(formatted_line, end='', flush=True)
        if self._list_stdout:
          self._streaming_queue.put(formatted_line)

  def _start_subprocess_and_reading_thread(self,
                                           task_type,
                                           task_id,
                                           cluster_spec=None,
                                           proc_func=None,
                                           args=None,
                                           kwargs=None):
    """Start a subprocess and a thread the reads lines from the subprocess."""

    if dill is None:
      raise unittest.SkipTest(
          'TODO(b/150264776): Resolve dependency issue in CI')

    test_env = TestEnvironment(
        task_type=task_type,
        task_id=task_id,
        cluster_spec=cluster_spec or self._cluster_spec,
        rpc_layer=self._rpc_layer,
        grpc_fail_fast=self._grpc_fail_fast,
        v2_enabled=self._v2_enabled,
        executing_eagerly=self._executing_eagerly,
    )
    pipe_r, pipe_w = multiprocessing.Pipe(duplex=False)
    resources = Resources(
        process_status_queue=self._process_status_queue,
        parent_to_sub_queue=self._parent_to_sub_queue,
        streaming_pipe_w=pipe_w,
        barrier=self._barrier,
    )
    if proc_func is None:
      proc_func, args, kwargs = self._proc_func, self._args, self._kwargs
    # Always use dill to pickle proc_func so that we support more callable
    # types, e.g. lambda.
    proc_func = dill.dumps(proc_func, dill.HIGHEST_PROTOCOL)
    if self._use_dill_for_args:
      args = dill.dumps(args, dill.HIGHEST_PROTOCOL)
      kwargs = dill.dumps(kwargs, dill.HIGHEST_PROTOCOL)

    p = _Process(
        test_env=test_env,
        target=_ProcFunc(),
        args=(resources, test_env, proc_func, args, kwargs,
              self._use_dill_for_args),
        daemon=self._daemon)
    p.start()
    self._processes[(task_type, task_id)] = p
    self._terminated.discard((task_type, task_id))

    # For each subprocess, we dedicate a thread continuously reading lines
    # from them.
    thread = threading.Thread(  # pylint: disable=unexpected-keyword-arg
        target=self._continuously_readline_from_sub,
        args=(pipe_r, task_type, task_id))
    thread.start()
    self._reading_threads.append(thread)

    if self._watchdog_thread is None or not self._watchdog_thread.is_alive():
      self._watchdog_thread = threading.Thread(target=self._process_watchdog)
      self._watchdog_thread.start()

  def start(self):
    """Starts processes, one for each task in `cluster_spec`.

    Note that this is best effort by the applicable multiprocessing library,
    and it may take up to seconds for a subprocess to be successfully started.
    """
    with self._process_lock:
      if self._processes:
        raise ValueError('MultiProcessRunner already started.')
      if self._joined:
        raise ValueError('cannot start new processes after'
                         'MultiProcessRunner.join() is called')

      for task_type, addresses in self._cluster_spec.items():
        for task_id, _ in enumerate(addresses):
          self._start_subprocess_and_reading_thread(task_type, task_id)

    # TODO(rchao): Remove the need of using SIGALRM if possible. At this time,
    # without this the tests become very flaky.
    if self._max_run_time is not None:

      def handler(signum, frame):
        del signum, frame
        self.terminate_all()

      signal.signal(signal.SIGALRM, handler)
      signal.alarm(self._max_run_time)

  def start_in_process_as(self, as_task_type, as_task_id):
    """Start the processes, with the specified task run in main process.

    This is similar to `start()` except that the task with task_type
    `as_task_type` and task_id `as_task_id` is run in the main process.
    This method is particularly useful when debugging tool such as `pdb` is
    needed in some specific task. Note that since this method is blocking until
    that specific task exits, additional actions would need a thread to be
    called:

    ```python
    def proc_func():
      # user code to be run
      import pdb; pdb.set_trace()

    def follow_ups():
      time.sleep(5)
      mpr.start_single_process(
          task_type='evaluator',
          task_id=0)

    mpr = multi_process_runner.MultiProcessRunner(
        proc_func,
        multi_worker_test_base.create_cluster_spec(
            has_chief=True, num_workers=1))
    threading.Thread(target=follow_ups).start()
    mpr.start_in_process_as(as_task_type='chief', as_task_id=0)
    mpr.join()
    ```

    Note that if `list_stdout=True`, the logs/stdout by task
    run by the main process is not available in result.stdout.

    Args:
      as_task_type: The task type to be run in the main process.
      as_task_id: The task id to be run in the main process.
    """
    if self._processes:
      raise ValueError('MultiProcessRunner already started.')
    with self._process_lock:
      if self._joined:
        raise ValueError('cannot start new processes after'
                         'MultiProcessRunner.join() is called')
      for task_type, addresses in self._cluster_spec.items():
        for task_id, _ in enumerate(addresses):
          if not (task_type == as_task_type and task_id == as_task_id):
            self._start_subprocess_and_reading_thread(task_type, task_id)

    _set_tf_config(as_task_type, as_task_id, self._cluster_spec,
                   self._rpc_layer)
    self._proc_func(*self._args, **self._kwargs)

  def start_single_process(self,
                           task_type,
                           task_id,
                           cluster_spec=None,
                           proc_func=None,
                           args=None,
                           kwargs=None):
    """Starts a single process.

    This starts a process in the cluster with the task type, task id, and the
    process function (`proc_func`). If process function is `None`, the function
    provided at `__init__` will be used. If `cluster_spec` is `None`, the
    cluster spec provided at `__init__` will be used.

    TODO(rchao): It is meant that all subprocesses will be updated with the new
    cluster spec, but this has yet to be implemented. At this time only the
    newly started subprocess picks up this updated cluster spec.

    Args:
      task_type: The task type.
      task_id: The task id.
      cluster_spec: The cluster spec to be used on the newly started
        process. If `None`, the cluster spec provided at `__init__` will be
        used.
      proc_func: The process function to be run on the newly started
        process. If specified, specify `args` and `kwargs` as well. If `None`,
        the function provided at `__init__` will be used.
      args: Optional positional arguments to be supplied in `proc_func`.
      kwargs: Optional keyword arguments to be supplied in `proc_func`.
    """
    with self._process_lock:
      if self._joined:
        raise ValueError('cannot start new processes after'
                         'MultiProcessRunner.join() is called')
      self._start_subprocess_and_reading_thread(
          task_type,
          task_id,
          cluster_spec=cluster_spec,
          proc_func=proc_func,
          args=args or (),
          kwargs=kwargs or {})

  def _queue_to_list(self, queue_to_convert):
    """Convert `queue.Queue` to `list`."""
    list_to_return = []
    # Calling `queue.empty()` is not reliable.
    while True:
      try:
        list_to_return.append(queue_to_convert.get(block=False))
      except Queue.Empty:
        break
    return list_to_return

  def _get_process_statuses(self):
    # One worker may have multiple statuses. We only keep the last one.
    statuses = {}
    for status in self._queue_to_list(self._process_status_queue):
      statuses[(status.task_type, status.task_id)] = status
    return statuses

  def get_process_id(self, task_type, task_id):
    """Returns the subprocess id given the task type and task id."""
    with self._process_lock:
      p = self._processes.get((task_type, task_id), None)
    return p.pid if p else None

  def get_process_exit_code(self, task_type, task_id):
    """Returns the subprocess exit code given the task type and task id.

    Args:
      task_type: The task type.
      task_id: The task id.

    Returns:
      The subprocess exit code; `None` if the subprocess has not exited yet.

    Raises:
      KeyError: If the corresponding subprocess is not found with `task_type`
        and `task_id`.
    """
    with self._process_lock:
      p = self._processes[(task_type, task_id)]
    return p.exitcode if p else None

  def process_exists(self, task_type, task_id):
    """Returns whether the subprocess still exists given the task type and id.

    Args:
      task_type: The task type.
      task_id: The task id.

    Returns:
      Boolean; whether the subprocess still exists. If the subprocess has
      exited, this returns False.
    """
    return self.get_process_exit_code(task_type, task_id) is None

  def _process_watchdog(self):
    """Simulates a cluster management system.

    - If auto_restart is True, it restarts processes that exit with a non-zero
      exit code. Note that when join() times out it overrides auto_restart to
      False.
    - If dependence_on_chief is True, it terminates all processes once the chief
      exits. If auto_restart is also True, it only terminates all processes if
      the chief exit with a zero exit code, otherwise it restarts the chief.

    This runs in self._watchdog_thread.
    """
    while True:
      time.sleep(1)
      with self._process_lock:
        chief = self._processes.get(('chief', 0), None)
        # Terminate the cluster when _dependence_on_chief is True if either:
        # - chief has exited with zero exit code.
        # - chief has exited with non-zero exit code and self._auto_restart is
        #   False.
        if chief and self._dependence_on_chief and chief.exitcode is not None:
          if chief.exitcode == 0 or (not self._auto_restart):
            for p in self._processes.values():
              # Give other processes a chance to exit on their own.
              p.join(timeout=3)
            self._terminate_all()
            for p in self._processes.values():
              p.join()
            return

        # Auto restart failed processes if self._auto_restart is True.
        if self._auto_restart:
          has_failure = False
          for (task_type, task_id), p in self._processes.items():
            if p.exitcode is not None and p.exitcode != 0:
              has_failure = True
              logging.info('Restarting failed %s-%d', task_type, task_id)
              self._start_subprocess_and_reading_thread(task_type, task_id)
          if has_failure:
            continue

        # Exit the thread if all processes have exited at this point.
        if all(p.exitcode is not None for p in self._processes.values()):
          return

  def _reraise_if_subprocess_error(self, process_statuses):
    for process_status in process_statuses.values():
      assert isinstance(process_status, _ProcessStatusInfo)
      if not process_status.is_successful:
        process_status.exc_info[1].mpr_result = self._get_mpr_result(
            process_statuses)
        six.reraise(*process_status.exc_info)

  def join(self, timeout=_DEFAULT_TIMEOUT_SEC):
    """Joins all the processes with timeout.

    If any of the subprocesses does not exit approximately after `timeout`
    seconds has passed after `join` call, this raises a
    `SubprocessTimeoutError`.

    Note: At timeout, it uses SIGTERM to terminate the subprocesses, in order to
    log the stack traces of the subprocesses when they exit. However, this
    results in timeout when the test runs with tsan (thread sanitizer); if tsan
    is being run on the test targets that rely on timeout to assert information,
    `MultiProcessRunner.terminate_all()` must be called after `join()`, before
    the test exits, so the subprocesses are terminated with SIGKILL, and data
    race is removed.

    Args:
      timeout: if set and not all processes report status within roughly
        `timeout` seconds, a `SubprocessTimeoutError` exception will be raised.

    Returns:
      A MultiProcessRunnerResult object, which has two attributes,
      `return_value` and `stdout`. `return_value` always contains the return
      values from the subprocesses. If `list_stdout` argument is True at
      `__init__`, `stdout` is available that contains a list of all messages
      from subprocesses' stdout and stderr.

    Raises:
      SubprocessTimeoutError: if not all processes report status approximately
        within `timeout` seconds. When this is raised, a
        `MultiProcessRunnerResult` object can be retrieved by
        `SubprocessTimeoutError`'s mpr_result attribute, which has the same
        structure as above 'Returns' section describes.
      UnexpectedSubprocessExitError: If any of the subprocesses did not exit
        properly (for example, they exit on SIGTERM or SIGKILL signal). When
        this is raised, a `MultiProcessRunnerResult` object can be retrieved by
        `UnexpectedSubprocessExitError`'s mpr_result attribute, which has the
        same structure as above 'Returns' section describes. If `max_run_time`
        is not `None`, it is expected that some subprocesses may be
        force-killed when `max_run_time` is up, and this is raised in those
        cases.
      Exception: if there is an Exception propagated from any subprocess. When
        this is raised, a `MultiProcessRunnerResult` object can be retrieved by
        `UnexpectedSubprocessExitError`'s mpr_result attribute, which has the
        same structure as above 'Returns' section describes.
    """
    with self._process_lock:
      if self._joined:
        raise ValueError("MultiProcessRunner can't be joined twice.")
      self._joined = True

    self._watchdog_thread.join(timeout)
    if self._watchdog_thread.is_alive():
      # Timeout. Force termination to dump worker processes stack trace.
      with self._process_lock:
        self._auto_restart = False
      logging.error('Timeout when joining for child processes. Terminating...')
      self.terminate_all(sig=signal.SIGTERM)
      # Wait for the processes to terminate by themselves first, so they have a
      # chance to dump stacktraces. After _FORCE_KILL_WAIT_SEC, we SIGKILL them.
      self._watchdog_thread.join(_FORCE_KILL_WAIT_SEC)
      if self._watchdog_thread.is_alive():
        logging.error('Timeout when waiting for child processes to '
                      'print stacktrace. Sending SIGKILL...')
        self.terminate_all()
        self._watchdog_thread.join()
      process_statuses = self._get_process_statuses()
      self._reraise_if_subprocess_error(process_statuses)
      raise SubprocessTimeoutError(
          'One or more subprocesses timed out, where timeout was set to {}s. '
          'Please change the `timeout` argument for '
          '`MultiProcessRunner.join()` or `multi_process_runner.run()` '
          'if it should be adjusted.'.format(timeout),
          self._get_mpr_result(process_statuses))

    for (task_type, task_id), p in self._processes.items():
      logging.info('%s-%d exit code: %s', task_type, task_id, p.exitcode)

    process_statuses = self._get_process_statuses()
    self._reraise_if_subprocess_error(process_statuses)

    # Checking all the processes that are expected to exit properly.
    for (task_type, task_id), p in self._processes.items():
      # Successfully exiting process has exit code 0. We ignore processes that
      # are terminated.
      assert p.exitcode is not None
      if (p.exitcode > 0 and (task_type, task_id) not in self._terminated):
        raise UnexpectedSubprocessExitError(
            'Subprocess %s-%d exited with exit code %s. See logs for details.'
            % (task_type, task_id, p.exitcode),
            self._get_mpr_result(process_statuses))

    logging.info('Joining log reading threads.')
    for thread in self._reading_threads:
      thread.join()
    logging.info('Joined log reading threads.')

    # Clear the alarm.
    signal.alarm(0)

    return self._get_mpr_result(process_statuses)

  def _get_mpr_result(self, process_statuses):
    stdout = self._queue_to_list(self._streaming_queue)
    return_values = []
    for process_status in process_statuses.values():
      if process_status.return_value is not None:
        return_values.append(process_status.return_value)
    return MultiProcessRunnerResult(stdout=stdout, return_value=return_values)

  def terminate(self, task_type, task_id):
    """Terminates the process with `task_type` and `task_id`.

    If auto_retart=True, the terminated task will be restarted unless the chief
    has already exited with zero exit code.

    Args:
      task_type: the task type.
      task_id: the task id.

    """
    with self._process_lock:
      p = self._processes.get((task_type, task_id), None)
      if p is None:
        raise ValueError('{}-{} does not exist'.format(task_type, task_id))
      self._terminated.add((task_type, task_id))
      # TODO(crccw): change to use Process.terminate() as well.
      self._parent_to_sub_queue.put('terminate {} {}'.format(
          task_type, task_id))
      p.join()

  def _terminate_all(self, sig=None):
    """Terminates all subprocesses.

    The caller is required to hold self._process_lock.

    Args:
      sig: the signal used to terminate the process. The default is SIGKILL.
    """

    # Use SIGKILL as default. In systems where that's unavailable such as
    # windows, use SIGTERM.
    sig = sig or getattr(signal, 'SIGKILL', signal.SIGTERM)
    for (task_type, task_id), p in self._processes.items():
      if p.exitcode is not None:
        continue
      try:
        os.kill(p.pid, sig)
        self._terminated.add((task_type, task_id))
        logging.info('%s-%d terminated with signal %r.', task_type, task_id,
                     sig)
      except ProcessLookupError:
        logging.info('Attempting to kill %s-%d but it does not exist.',
                     task_type, task_id)

  def terminate_all(self, sig=None):
    """Terminates all subprocesses."""
    with self._process_lock:
      self._terminate_all(sig)


class _Process(multi_process_lib.Process):
  """A modified `multiprocessing.Process` that can set up environment variables."""

  # TODO(crccw): consider moving other logics in _ProcFunc to _Process.

  def __init__(self, test_env, **kwargs):
    super(_Process, self).__init__(**kwargs)
    self._test_env = test_env
    self._actual_run = getattr(self, 'run')
    self.run = self._run_with_setenv

  def _run_with_setenv(self):
    # We need to set environment variables before doing anything because
    # setenv() is not thread-safe.
    test_env = self._test_env
    if test_env.grpc_fail_fast is not None:
      os.environ['GRPC_FAIL_FAST'] = str(test_env.grpc_fail_fast)
    _set_tf_config(test_env.task_type, test_env.task_id, test_env.cluster_spec,
                   test_env.rpc_layer)
    return self._actual_run()


class _ProcFunc(object):
  """Represents a callable to run in a subprocess."""

  @contextlib.contextmanager
  def _runtime_mode(self, executing_eagerly):
    if executing_eagerly:
      with context.eager_mode():
        yield
    else:
      with context.graph_mode():
        yield

  def _message_checking_func(self, task_type, task_id):
    """A function that regularly checks messages from parent process."""
    # TODO(rchao): Remove this once parent uses SIGKILL to terminate subprocess.
    while True:
      try:
        message = self._resources.parent_to_sub_queue.get(block=False)

        # Currently the only possible message is termination.
        if not message.startswith('terminate'):
          raise ValueError('Unrecognized message: {}'.format(message))

        if message == 'terminate {} {}'.format(task_type, task_id):
          break
        else:
          # If the message is not targeting this process, put it back to the
          # queue.
          self._resources.parent_to_sub_queue.put(message)
          time.sleep(1)
      except Queue.Empty:
        time.sleep(0.1)
    self._resources.process_status_queue.put(
        _ProcessStatusInfo(
            task_type=task_type,
            task_id=task_id,
            is_successful=True,
            exc_info=None,
            return_value=None))
    # `os._exit(1)` is used to more reliably terminate a subprocess.
    os._exit(1)  # pylint: disable=protected-access

  def _close_streaming(self):
    """Close stdout, stderr and streaming pipe.

    We need to explicitly close them since Tensorflow may take a while to exit,
    so that the reading threads in the main process can exit more quickly.
    """
    sys.stdout.flush()
    sys.stderr.flush()
    sys.stdout.close()
    sys.stderr.close()
    self._resources.streaming_pipe_w.close()

  def __call__(self, resources, test_env, proc_func, args, kwargs,
               use_dill_for_args):
    """The wrapper function that actually gets run in child process(es)."""

    global _barrier

    self._resources = resources
    _barrier = self._resources.barrier
    proc_func = dill.loads(proc_func)
    if use_dill_for_args:
      args = dill.loads(args)
      kwargs = dill.loads(kwargs)

    if faulthandler is not None:
      faulthandler.enable()
      faulthandler.register(signal.SIGTERM, chain=True)

    # All logging should go to stderr to be streamed to the main process.
    logging.set_stderrthreshold(logging.DEBUG)

    # Assign sys.stdout and sys.stderr as duplicates of `streaming_pipe_w` so
    # print() and logging.*() write directly to `streaming_pipe_w`.
    # Unfortunately since we cannot prepend task_type and task_id information to
    # the streamed logs we will need a thread per subprocess to distinguish
    # where the piece of message is from.
    os.dup2(resources.streaming_pipe_w.fileno(), sys.stdout.fileno())
    os.dup2(resources.streaming_pipe_w.fileno(), sys.stderr.fileno())

    pid = os.getpid()
    logging.info('Subprocess with PID %d (%s, %d) is now being started.', pid,
                 test_env.task_type, test_env.task_id)

    # The thread will be dedicated to checking messages from the parent process.
    threading.Thread(  # pylint: disable=unexpected-keyword-arg
        target=self._message_checking_func,
        args=(test_env.task_type, test_env.task_id),
        daemon=True).start()

    if test_env.v2_enabled:
      v2_compat.enable_v2_behavior()

    with self._runtime_mode(test_env.executing_eagerly):
      info = _run_contained(test_env.task_type, test_env.task_id, proc_func,
                            args, kwargs)
      self._resources.process_status_queue.put(info)

      # Re-raise the exception in addition to reporting it to the parent
      # process, so that even if `--test_timeout` flag is set and the
      # error doesn't make it to be shown in parent process before bazel's
      # timeout, the log would still show what happens in this subprocess,
      # instead of silently suppressing the error due to early bazel
      # timeout. Raising an error in the subprocess produces stack trace in
      # the log, but the program continues running.
      if not info.is_successful:
        six.reraise(*info.exc_info)

      self._close_streaming()

    # Exit with code 0 as it's considered successful exit at this point.
    sys.exit(0)


class MultiProcessPoolRunner(object):
  """A utility class to start a process pool to simulate a cluster.

  It's similar to MultiProcessRunner, but uses a pool of processes to avoid the
  expensive initialization cost of Tensorflow.
  """

  def __init__(self, cluster_spec, initializer=None):
    """Creates a multi-process pool runner.

    Args:
      cluster_spec: Dict for cluster spec. The following is an example of
        cluster with three workers.
        {"worker": ["worker0.example.com:2222",
                    "worker1.example.com:2222",
                    "worker2.example.com:2222"]}
      initializer: a callable to called at the startup of worker processes.

    Raises:
      RuntimeError: if `multi_process_runner.test_main()` is not called.
      ValueError: if there are more than one chief in the `cluster_spec`.
    """
    self._cluster_spec = cluster_spec
    self._initializer = initializer
    self._conn = {}
    self._runner = None

  def __del__(self):
    self.shutdown()

  def shutdown(self):
    """Shuts down the worker pool."""
    for conn in self._conn.values():
      conn.close()
    self._conn = {}
    if self._runner is not None:
      self._runner.join()
      self._runner = None

  def _start(self):
    """Starts the worker pool."""
    # We need different arguments for different processes so we're passing a
    # no-op proc_func here and use start_single_process instead.
    #
    # We also need to start the process pool as daemon, so that they don't block
    # the program from exiting. Note that __del__ may not get called when
    # there's an exception. The user may also store a pool runner in a global
    # object to share across test cases

    if dill is None:
      raise unittest.SkipTest(
          'TODO(b/150264776): Resolve dependency issue in CI')

    self._runner = MultiProcessRunner(
        proc_func=lambda: None,
        cluster_spec=self._cluster_spec,
        use_dill_for_args=False,
        daemon=True)
    if self._initializer:
      initializer = dill.dumps(self._initializer, dill.HIGHEST_PROTOCOL)
    else:
      initializer = None
    for task_type, addresses in self._cluster_spec.items():
      for task_id, _ in enumerate(addresses):
        conn1, conn2 = multiprocessing.Pipe(duplex=True)
        self._conn[(task_type, task_id)] = conn1
        self._runner.start_single_process(
            task_type,
            task_id,
            proc_func=_pool_runner_worker,
            args=(task_type, task_id, initializer, conn2))

  def run(self, proc_func, args=None, kwargs=None):
    """Runs `proc_func` with `args` and `kwargs` on all jobs.

    Args:
      proc_func: The function to be run.
      args: Optional positional arguments to be supplied in `proc_func`.
      kwargs: Optional keyword arguments to be supplied in `proc_func`.

    Returns:
      A list of return values.
    """
    # TODO(b/150264776): skip in OSS until it's implemented.
    multi_process_lib.Process()
    if self._runner is None:
      self._start()

    proc_func = dill.dumps(proc_func, dill.HIGHEST_PROTOCOL)
    for conn in self._conn.values():
      conn.send((proc_func, args or [], kwargs or {}))

    process_statuses = []
    for (task_type, task_id), conn in self._conn.items():
      logging.info('Waiting for the result from %s-%d', task_type, task_id)
      try:
        process_statuses.append(conn.recv())
      except EOFError:
        # This shouldn't happen due to exceptions in proc_func. This usually
        # means bugs in the runner.
        self.shutdown()
        raise RuntimeError('Unexpected EOF. Worker process may have died. '
                           'Please report a bug')

    return_values = []
    for process_status in process_statuses:
      assert isinstance(process_status, _ProcessStatusInfo)
      if not process_status.is_successful:
        six.reraise(*process_status.exc_info)
      if process_status.return_value is not None:
        return_values.append(process_status.return_value)

    return return_values


def _pool_runner_worker(task_type, task_id, initializer, conn):
  """Function that runs on the workers in a pool.

  It listens for callables to run and returns the result until `conn` is closed.
  It captures the exceptions during executing the callable and return it through
  `conn`.

  Args:
    task_type: the task type.
    task_id: the task index.
    initializer: a callable to execute during startup.
    conn: a multiprocessing.Connection object to listen for tasks and send
      results.
  """
  if initializer:
    initializer = dill.loads(initializer)
    initializer()
  while True:
    try:
      proc_func, args, kwargs = conn.recv()
    except EOFError:
      break
    proc_func = dill.loads(proc_func)
    info = _run_contained(task_type, task_id, proc_func, args, kwargs)
    sys.stdout.flush()
    sys.stderr.flush()
    conn.send(info)


def _run_contained(task_type, task_id, proc_func, args, kwargs):
  """Runs `proc_func` with `args` and `kwargs`.

  The function returns _ProcessStatusInfo which captures the return value and
  the exception.

  Args:
    task_type: the task type.
    task_id: the task index.
    proc_func: the function to be run.
    args: optional positional arguments to be supplied in `proc_func`.
    kwargs: optional keyword arguments to be supplied in `proc_func`.

  Returns:
    a _ProcessStatusInfo.

  """
  is_successful = False
  return_value = None
  exc_info = None
  try:
    return_value = proc_func(*args, **kwargs)
    is_successful = True
    return _ProcessStatusInfo(
        task_type=task_type,
        task_id=task_id,
        is_successful=is_successful,
        exc_info=exc_info,
        return_value=return_value)

  # If `proc_func` ends up exiting with `sys.exit()`, the `SystemExit` is not
  # handled here.
  except Exception:  # pylint: disable=broad-except
    exc_info = sys.exc_info()
    return _ProcessStatusInfo(
        task_type=task_type,
        task_id=task_id,
        is_successful=is_successful,
        exc_info=exc_info,
        return_value=return_value)


class SubprocessTimeoutError(RuntimeError):
  """An error that indicates there is at least one subprocess timing out.

  When this is raised, a `MultiProcessRunnerResult` object can be retrieved by
  `SubprocessTimeoutError`'s mpr_result attribute. See
  `MultiProcessRunner.join()` for more information.
  """

  def __init__(self, msg, mpr_result):
    super(SubprocessTimeoutError, self).__init__(msg)
    self.mpr_result = mpr_result


class UnexpectedSubprocessExitError(RuntimeError):
  """An error indicating there is at least one subprocess with unexpected exit.

  When this is raised, a `MultiProcessRunnerResult` object can be retrieved by
  `UnexpectedSubprocessExitError`'s mpr_result attribute. See
  `MultiProcessRunner.join()` for more information.
  """

  def __init__(self, msg, mpr_result):
    super(UnexpectedSubprocessExitError, self).__init__(msg)
    self.mpr_result = mpr_result


class MultiProcessRunnerNotInitializedError(RuntimeError):
  """An error indicating `MultiProcessRunner` is used without initialization.

  When this is raised, user is supposed to call
  `multi_process_runner.test_main()` within `if __name__ == '__main__':` block
  to properly initialize `multi_process_runner`.
  """
  pass


def _set_tf_config(task_type, task_id, cluster_spec, rpc_layer=None):
  """Set TF_CONFIG environment variable."""
  tf_config_dict = {
      'cluster': cluster_spec,
      'task': {
          'type': task_type,
          'index': task_id,
      },
  }
  if rpc_layer is not None:
    tf_config_dict['rpc_layer'] = rpc_layer
  os.environ['TF_CONFIG'] = json.dumps(tf_config_dict)


def run(proc_func,
        cluster_spec,
        rpc_layer=None,
        max_run_time=None,
        grpc_fail_fast=None,
        stream_stdout=True,
        list_stdout=False,
        timeout=_DEFAULT_TIMEOUT_SEC,
        args=None,
        kwargs=None):  # pylint: disable=g-doc-args
  """Runs functions in local child processes.

  It is a convenience method that creates a `MultiProcessRunner` object and
  invokes `start` and `join` method. Please see these methods for detailed
  documentations.

  Returns:
    A MultiProcessRunnerResult object returned from `MultiProcessRunner.join()`.
  """
  runner = MultiProcessRunner(
      proc_func,
      cluster_spec,
      rpc_layer,
      max_run_time=max_run_time,
      grpc_fail_fast=grpc_fail_fast,
      stream_stdout=stream_stdout,
      list_stdout=list_stdout,
      args=args,
      kwargs=kwargs)
  runner.start()
  return runner.join(timeout)


# This is set by MultiProcessRunner in worker processes.
_barrier = None


def barrier():
  if _barrier is None:
    raise ValueError(
        'barrier is not defined. It is likely because you are calling barrier()'
        'in the main process. barrier() can only be called in the subprocesses.'
    )
  return _barrier


_manager = None
_manager_lock = threading.Lock()


def manager():
  """Returns the multiprocessing manager object for concurrency tools.

  The manager object is useful as it controls a server process that holds
  the python objects that can be shared across processes. This can be used
  for parent-subprocess communication:

  ```python
  manager = multi_process_runner.manager()
  some_event_happening_in_subprocess = manager.Event()
  mpr = multi_process_runner.MultiProcessRunner(proc_func, cluster_spec,
      args=(some_event_happening_in_subprocess,))
  mpr.start()
  some_event_happening_in_subprocess.wait()
  # Do something that only should after some event happens in subprocess.
  ```

  Note that the user of multi_process_runner should not create additional
  `multiprocessing.Manager()` objects; doing so can result in segfault in
  some cases.

  This method should only be called after multi_process_runner.test_main() is
  called.
  """
  global _manager
  with _manager_lock:
    if _manager is None:
      _manager = multiprocessing.Manager()
    return _manager


def test_main():
  """Main function to be called within `__main__` of a test file."""
  multi_process_lib.test_main()
