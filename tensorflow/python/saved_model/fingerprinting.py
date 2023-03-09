# Copyright 2022 The TensorFlow Authors. All Rights Reserved.
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
"""Methods for SavedModel fingerprinting.

This module contains classes and functions for reading the SavedModel
fingerprint.
"""

from tensorflow.core.protobuf import fingerprint_pb2
from tensorflow.python.saved_model.pywrap_saved_model import fingerprinting as fingerprinting_pywrap
from tensorflow.python.util.tf_export import tf_export


@tf_export("saved_model.experimental.Fingerprint", v1=[])
class Fingerprint(object):
  """The SavedModel fingerprint.

  Each attribute of this class is named after a field name in the
  FingerprintDef proto and contains the value of the respective field in the
  protobuf.

  Attributes:
    saved_model_checksum: A uint64 containing the `saved_model_checksum`.
    graph_def_program_hash: A uint64 containing `graph_def_program_hash`.
    signature_def_hash: A uint64 containing the `signature_def_hash`.
    saved_object_graph_hash: A uint64 containing the `saved_object_graph_hash`.
    checkpoint_hash: A uint64 containing the`checkpoint_hash`.
    version: An int32 containing the producer field of the VersionDef.
  """

  def __init__(
      self,
      saved_model_checksum=None,
      graph_def_program_hash=None,
      signature_def_hash=None,
      saved_object_graph_hash=None,
      checkpoint_hash=None,
      version=None,
  ):
    """Initializes the instance based on values in the SavedModel fingerprint.

    Args:
      saved_model_checksum: Value of the`saved_model_checksum`.
      graph_def_program_hash: Value of the `graph_def_program_hash`.
      signature_def_hash: Value of the `signature_def_hash`.
      saved_object_graph_hash: Value of the `saved_object_graph_hash`.
      checkpoint_hash: Value of the `checkpoint_hash`.
      version: Value of the producer field of the VersionDef.
    """
    self.saved_model_checksum = saved_model_checksum
    self.graph_def_program_hash = graph_def_program_hash
    self.signature_def_hash = signature_def_hash
    self.saved_object_graph_hash = saved_object_graph_hash
    self.checkpoint_hash = checkpoint_hash
    self.version = version

  @classmethod
  def from_proto(cls, proto):
    return Fingerprint(
        proto.saved_model_checksum,
        proto.graph_def_program_hash,
        proto.signature_def_hash,
        proto.saved_object_graph_hash,
        proto.checkpoint_hash,
        proto.version)


@tf_export("saved_model.experimental.read_fingerprint", v1=[])
def read_fingerprint(export_dir):
  """Reads the fingerprint of a SavedModel in `export_dir`.

  Returns a `tf.saved_model.experimental.Fingerprint` object that contains
  the values of the SavedModel fingerprint, which is persisted on disk in the
  `fingerprint.pb` file in the `export_dir`.

  Read more about fingerprints in the SavedModel guide at
  https://www.tensorflow.org/guide/saved_model.

  Args:
    export_dir: The directory that contains the SavedModel.

  Returns:
    A `tf.saved_model.experimental.Fingerprint`.

  Raises:
    FingerprintException: If no or an invalid fingerprint is found.
  """
  try:
    fingerprint = fingerprinting_pywrap.ReadSavedModelFingerprint(export_dir)
  except fingerprinting_pywrap.FingerprintException as e:
    raise FileNotFoundError(f"SavedModel Fingerprint Error: {e}") from None  # pylint: disable=raise-missing-from
  return Fingerprint.from_proto(
      fingerprint_pb2.FingerprintDef().FromString(fingerprint))
