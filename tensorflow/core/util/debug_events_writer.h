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

#ifndef TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_
#define TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_

#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/io/record_writer.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/protobuf/debug_event.pb.h"

namespace tensorflow {

// The set of files generated by a debugged TensorFlow program.
enum DebugEventFileType {
  METADATA,
  SOURCE_FILES,
  STACK_FRAMES,
  GRAPHS,
  EXECUTION,
  GRAPH_EXECUTION_TRACES,
};

class DebugEventsWriter {
 public:
#ifndef SWIG
  // Prefix of version string present in the first entry of every event file.
  static constexpr const char* kFileNamePrefix = "tfdbg_events";
  static constexpr const char* kMetadataSuffix = "metadata";
  static constexpr const char* kSourceFilesSuffix = "source_files";
  static constexpr const char* kStackFramesSuffix = "stack_frames";
  static constexpr const char* kGraphsSuffix = "graphs";
  static constexpr const char* kExecutionSuffix = "execution";
  static constexpr const char* kGraphExecutionTracesSuffix =
      "graph_execution_traces";

  static constexpr const char* kVersionPrefix = "debug.Event:";
  static constexpr const int kCurrentFormatVersion = 1;
#endif

  // Get the DebugEventsWriter for the given dump_root.
  // For a given dump_root value, it is a singleton. tfdbg event files come in
  // sets of six. The singleton pattern avoids storing multiple sets in a single
  // folder, which might cause confusion.
  static DebugEventsWriter* GetDebugEventsWriter(const string& dump_root);

  // Get the size of the cyclic buffer for execution data (in number
  // of DebugEvent protos).
  static const int64 GetDebugEventsWriterExecutionBufferSize();

  // Set the size of the cyclic buffer for execution data (in number
  // of DebugEvent protos). Applies to all dump roots that will be
  // created after this call. Setting it to a negative number abolishes
  // the cyclic-buffer behavior.
  static void SetDebugEventsWriterExecutionBufferSize(int64 size);

  ~DebugEventsWriter();

  // Sets the debug event filenames and opens file for writing.
  // All files (see the DebugEventFileType enum) share the same prefix and
  // differ only in their suffixes. If not called by user, will be invoked
  // automatically by a call to FileName() or any of the Write*() methods().
  // Idempotent: if the metadata file exists and is open, this is a no-op.
  // If on the other hand the file was opened, but has since disappeared (e.g.
  // deleted by another process), this will open a new file.
  Status Init();

  // The four DebugEvent fields below are written _without_ the cyclic buffer.
  // Source file contents are written to the *.source_files file.
  void WriteSourceFile(const SourceFile& source_file);
  // Stack frames are written to the *.code_locations file.
  void WriteStackFrameWithId(const StackFrameWithId& stack_frame_with_id);
  // Graph op creation events are written to the *.graphs file.
  void WriteGraphOpCreation(const GraphOpCreation& graph_op_creation);
  // Debugged graphs are written to the *.graphs file.
  void WriteDebuggedGraph(const DebuggedGraph& debugged_graph);

  // The two DebugEvent fields below are written to the cyclic buffer
  // and saved to disk only at the FlushExecutionFiles() call.
  // Execution events (eager execution of an op or a tf.function) are written to
  // the *.execution file.
  void WriteEagerExecution(const Execution& execution);
  // Graph execution traces (graph-internal tensor values or their summaries)
  // are written to the *.graph_execution_traces file.
  void WriteGraphExecutionTrace(
      const GraphExecutionTrace& graph_execution_trace);

  // EventWriter automatically flushes and closes on destruction, but
  // this method is provided for users who want to write to disk sooner
  // and/or check for success.
  // FlushNonExecutionFiles() pushes outstanding DebugEvents not written
  // events to the cyclic buffer to their respective files.
  Status FlushNonExecutionFiles();

  // Writes current contents of the cyclic buffers to their respective
  // debug event files and clears the cyclic buffers.
  Status FlushExecutionFiles();

  // Close() calls FlushNonExecutionFiles() and FlushExecutionFiles()
  // and then closes the current debug events files.
  Status Close();

 private:
  // Blocks access to constructor for singleton pattern.
  DebugEventsWriter(const string& dump_root);

  // Get the path prefix. The same for all files, which differ only in the
  // suffix.
  string FileName(DebugEventFileType type);

  TF_DISALLOW_COPY_AND_ASSIGN(DebugEventsWriter);
}

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_UTIL_DEBUG_EVENTS_WRITER_H_
