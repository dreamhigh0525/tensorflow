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

"""Support for training models.

See the @{$python/train} guide.

@@Optimizer
@@GradientDescentOptimizer
@@AdadeltaOptimizer
@@AdagradOptimizer
@@AdagradDAOptimizer
@@MomentumOptimizer
@@AdamOptimizer
@@FtrlOptimizer
@@ProximalGradientDescentOptimizer
@@ProximalAdagradOptimizer
@@RMSPropOptimizer
@@custom_gradient
@@gradients
@@AggregationMethod
@@GradientTape
@@stop_gradient
@@hessians
@@clip_by_value
@@clip_by_norm
@@clip_by_average_norm
@@clip_by_global_norm
@@global_norm
@@cosine_decay
@@cosine_decay_restarts
@@linear_cosine_decay
@@noisy_linear_cosine_decay
@@exponential_decay
@@inverse_time_decay
@@natural_exp_decay
@@piecewise_constant
@@polynomial_decay
@@ExponentialMovingAverage
@@Coordinator
@@QueueRunner
@@LooperThread
@@add_queue_runner
@@start_queue_runners
@@Server
@@Supervisor
@@SessionManager
@@ClusterSpec
@@replica_device_setter
@@MonitoredTrainingSession
@@MonitoredSession
@@SingularMonitoredSession
@@Scaffold
@@SessionCreator
@@ChiefSessionCreator
@@WorkerSessionCreator
@@summary_iterator
@@SessionRunHook
@@SessionRunArgs
@@SessionRunContext
@@SessionRunValues
@@LoggingTensorHook
@@StopAtStepHook
@@CheckpointSaverHook
@@CheckpointSaverListener
@@NewCheckpointReader
@@StepCounterHook
@@NanLossDuringTrainingError
@@NanTensorHook
@@SummarySaverHook
@@GlobalStepWaiterHook
@@FinalOpsHook
@@FeedFnHook
@@ProfilerHook
@@SecondOrStepTimer
@@global_step
@@basic_train_loop
@@get_global_step
@@get_or_create_global_step
@@create_global_step
@@assert_global_step
@@write_graph
@@load_checkpoint
@@load_variable
@@list_variables
@@init_from_checkpoint
@@warm_start
@@VocabInfo
"""

# Optimizers.
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

# pylint: disable=g-bad-import-order,unused-import
from tensorflow.python.ops.sdca_ops import sdca_optimizer
from tensorflow.python.ops.sdca_ops import sdca_fprint
from tensorflow.python.ops.sdca_ops import sdca_shrink_l1
from tensorflow.python.training.adadelta import AdadeltaOptimizer
from tensorflow.python.training.adagrad import AdagradOptimizer
from tensorflow.python.training.adagrad_da import AdagradDAOptimizer
from tensorflow.python.training.proximal_adagrad import ProximalAdagradOptimizer
from tensorflow.python.training.adam import AdamOptimizer
from tensorflow.python.training.ftrl import FtrlOptimizer
from tensorflow.python.training.momentum import MomentumOptimizer
from tensorflow.python.training.moving_averages import ExponentialMovingAverage
from tensorflow.python.training.optimizer import Optimizer
from tensorflow.python.training.rmsprop import RMSPropOptimizer
from tensorflow.python.training.gradient_descent import GradientDescentOptimizer
from tensorflow.python.training.proximal_gradient_descent import ProximalGradientDescentOptimizer
from tensorflow.python.training.sync_replicas_optimizer import SyncReplicasOptimizer

# Utility classes for training.
from tensorflow.python.training.coordinator import Coordinator
from tensorflow.python.training.coordinator import LooperThread
# go/tf-wildcard-import
# pylint: disable=wildcard-import
from tensorflow.python.training.queue_runner import *

# For the module level doc.
from tensorflow.python.training import input as _input
from tensorflow.python.training.input import *  # pylint: disable=redefined-builtin
# pylint: enable=wildcard-import

from tensorflow.python.training.basic_session_run_hooks import SecondOrStepTimer
from tensorflow.python.training.basic_session_run_hooks import LoggingTensorHook
from tensorflow.python.training.basic_session_run_hooks import StopAtStepHook
from tensorflow.python.training.basic_session_run_hooks import CheckpointSaverHook
from tensorflow.python.training.basic_session_run_hooks import CheckpointSaverListener
from tensorflow.python.training.basic_session_run_hooks import StepCounterHook
from tensorflow.python.training.basic_session_run_hooks import NanLossDuringTrainingError
from tensorflow.python.training.basic_session_run_hooks import NanTensorHook
from tensorflow.python.training.basic_session_run_hooks import SummarySaverHook
from tensorflow.python.training.basic_session_run_hooks import GlobalStepWaiterHook
from tensorflow.python.training.basic_session_run_hooks import FinalOpsHook
from tensorflow.python.training.basic_session_run_hooks import FeedFnHook
from tensorflow.python.training.basic_session_run_hooks import ProfilerHook
from tensorflow.python.training.basic_loops import basic_train_loop
from tensorflow.python.training.checkpointable_utils import Checkpoint
from tensorflow.python.training.checkpoint_utils import init_from_checkpoint
from tensorflow.python.training.checkpoint_utils import list_variables
from tensorflow.python.training.checkpoint_utils import load_checkpoint
from tensorflow.python.training.checkpoint_utils import load_variable

from tensorflow.python.training.device_setter import replica_device_setter
from tensorflow.python.training.monitored_session import Scaffold
from tensorflow.python.training.monitored_session import MonitoredTrainingSession
from tensorflow.python.training.monitored_session import SessionCreator
from tensorflow.python.training.monitored_session import ChiefSessionCreator
from tensorflow.python.training.monitored_session import WorkerSessionCreator
from tensorflow.python.training.monitored_session import MonitoredSession
from tensorflow.python.training.monitored_session import SingularMonitoredSession
from tensorflow.python.training.saver import Saver
from tensorflow.python.training.saver import checkpoint_exists
from tensorflow.python.training.saver import generate_checkpoint_state_proto
from tensorflow.python.training.saver import get_checkpoint_mtimes
from tensorflow.python.training.saver import get_checkpoint_state
from tensorflow.python.training.saver import latest_checkpoint
from tensorflow.python.training.saver import update_checkpoint_state
from tensorflow.python.training.saver import export_meta_graph
from tensorflow.python.training.saver import import_meta_graph
from tensorflow.python.training.session_run_hook import SessionRunHook
from tensorflow.python.training.session_run_hook import SessionRunArgs
from tensorflow.python.training.session_run_hook import SessionRunContext
from tensorflow.python.training.session_run_hook import SessionRunValues
from tensorflow.python.training.session_manager import SessionManager
from tensorflow.python.training.summary_io import summary_iterator
from tensorflow.python.training.supervisor import Supervisor
from tensorflow.python.training.training_util import write_graph
from tensorflow.python.training.training_util import global_step
from tensorflow.python.training.training_util import get_global_step
from tensorflow.python.training.training_util import assert_global_step
from tensorflow.python.training.training_util import create_global_step
from tensorflow.python.training.training_util import get_or_create_global_step
from tensorflow.python.training.warm_starting_util import VocabInfo
from tensorflow.python.training.warm_starting_util import warm_start
from tensorflow.python.pywrap_tensorflow import do_quantize_training_on_graphdef
from tensorflow.python.pywrap_tensorflow import NewCheckpointReader
from tensorflow.python.util.tf_export import tf_export

# pylint: disable=wildcard-import
# Training data protos.
from tensorflow.core.example.example_pb2 import *
from tensorflow.core.example.feature_pb2 import *
from tensorflow.core.protobuf.saver_pb2 import *

# Utility op.  Open Source. TODO(touts): move to nn?
from tensorflow.python.training.learning_rate_decay import *
# pylint: enable=wildcard-import

# Distributed computing support.
from tensorflow.core.protobuf.cluster_pb2 import ClusterDef
from tensorflow.core.protobuf.cluster_pb2 import JobDef
from tensorflow.core.protobuf.tensorflow_server_pb2 import ServerDef
from tensorflow.python.training.server_lib import ClusterSpec
from tensorflow.python.training.server_lib import Server

# pylint: disable=undefined-variable
tf_export("train.BytesList")(BytesList)
tf_export("train.ClusterDef")(ClusterDef)
tf_export("train.Example")(Example)
tf_export("train.Feature")(Feature)
tf_export("train.Features")(Features)
tf_export("train.FeatureList")(FeatureList)
tf_export("train.FeatureLists")(FeatureLists)
tf_export("train.FloatList")(FloatList)
tf_export("train.Int64List")(Int64List)
tf_export("train.JobDef")(JobDef)
tf_export("train.SaverDef")(SaverDef)
tf_export("train.SequenceExample")(SequenceExample)
tf_export("train.ServerDef")(ServerDef)
# pylint: enable=undefined-variable
