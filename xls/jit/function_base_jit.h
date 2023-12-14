// Copyright 2022 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef XLS_JIT_FUNCTION_BASE_JIT_H_
#define XLS_JIT_FUNCTION_BASE_JIT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/ir/events.h"
#include "xls/ir/function.h"
#include "xls/ir/function_base.h"
#include "xls/ir/node.h"
#include "xls/ir/proc.h"
#include "xls/jit/ir_builder_visitor.h"
#include "xls/jit/jit_buffer.h"
#include "xls/jit/jit_channel_queue.h"
#include "xls/jit/jit_runtime.h"
#include "xls/jit/orc_jit.h"

namespace xls {

// Type alias for the jitted functions implementing XLS FunctionBases. Argument
// descriptions:
//   inputs: array of pointers to input buffers (e.g., parameter values). Note
//        that for Block* functions specifically the inputs are all the input
//        ports followed by all the registers.
//   outputs: array of pointers to output buffers (e.g., function return value,
//        proc next state values). Note that for Block* specifically the outputs
//        are all the output-ports followed by all the new register values.
//   temp_buffer: heap-allocated scratch space for the JITed funcion. This
//       buffer hold temporary node values which cannot be stack allocated via
//       allocas.
//   events: pointer to events objects which records information from
//       instructions like trace.
//   user_data: pointer to arbitrary data passed to send/receive functions
//       in procs.
//   jit_runtime: pointer to a JitRuntime object.
//   continuation_point: an opaque value indicating the point in the
//      FunctionBase to start execution when the jitted function is called.
//      Used to enable interruption and resumption of execution of the
//      the FunctionBase due to blocking operations such as receives.
//
// Returns the continuation point at which execution stopped or 0 if the tick
// completed.
using JitFunctionType = int64_t (*)(const uint8_t* const* inputs,
                                    uint8_t* const* outputs, void* temp_buffer,
                                    InterpreterEvents* events, void* user_data,
                                    JitRuntime* jit_runtime,
                                    int64_t continuation_point);

// Abstraction holding function pointers and metadata about a jitted function
// implementing a XLS Function, Proc, etc.
class JittedFunctionBase {
 public:
  // Builds and returns an LLVM IR function implementing the given XLS
  // function.
  static absl::StatusOr<JittedFunctionBase> Build(Function* xls_function,
                                                  OrcJit& orc_jit);

  // Builds and returns an LLVM IR function implementing the given XLS
  // proc.
  static absl::StatusOr<JittedFunctionBase> Build(
      Proc* proc, JitChannelQueueManager* queue_mgr, OrcJit& orc_jit);

  // Builds and returns an LLVM IR function implementing the given XLS
  // block.
  static absl::StatusOr<JittedFunctionBase> Build(Block* block, OrcJit& jit);

  // Create a buffer with space for all inputs, correctly aligned.
  JitArgumentSet CreateInputBuffer() const;

  // Create a buffer with space for all outputs, correctly aligned.
  JitArgumentSet CreateOutputBuffer() const;

  // Return if the required alignments and sizes of both the inputs and outputs
  // are identical.
  bool InputsAndOutputsAreEquivalent() const {
    return absl::c_equal(input_buffer_sizes_, output_buffer_sizes_) &&
           absl::c_equal(input_buffer_prefered_alignments_,
                         output_buffer_prefered_alignments_);
  }

  // Create a buffer capable of being used for both the input and output of a
  // jitted function.
  //
  // Returns an error if `InputsAndOutputsAreEquivalent()` is not true.
  absl::StatusOr<JitArgumentSet> CreateInputOutputBuffer() const;

  // Create a buffer usable as the temporary storage, correctly aligned.
  JitTempBuffer CreateTempBuffer() const;

  // Execute the actual function (after verifying some invariants)
  int64_t RunJittedFunction(const JitArgumentSet& inputs,
                            JitArgumentSet& outputs, JitTempBuffer& temp_buffer,
                            InterpreterEvents* events, void* user_data,
                            JitRuntime* jit_runtime,
                            int64_t continuation_point) const;

  // Execute the jitted function using inputs not created by this function.
  // If kForceZeroCopy is false the inputs will be memcpy'd if needed to aligned
  // temporary buffers.
  template<bool kForceZeroCopy = false>
  int64_t RunUnalignedJittedFunction(const uint8_t* const* inputs,
                                     uint8_t* const* outputs, void* temp_buffer,
                                     InterpreterEvents* events, void* user_data,
                                     JitRuntime* jit_runtime,
                                     int64_t continuation) const;

  // Execute the actual function (after verifying some invariants)
  std::optional<int64_t> RunPackedJittedFunction(
      const uint8_t* const* inputs, uint8_t* const* outputs, void* temp_buffer,
      InterpreterEvents* events, void* user_data, JitRuntime* jit_runtime,
      int64_t continuation_point) const;

  // Checks if we have a packed version of the function.
  bool HasPackedFunction() const { return packed_function_.has_value(); }

  std::string_view function_name() const { return function_name_; }

  absl::Span<int64_t const> input_buffer_sizes() const {
    return input_buffer_sizes_;
  }

  absl::Span<int64_t const> output_buffer_sizes() const {
    return output_buffer_sizes_;
  }

  absl::Span<int64_t const> packed_input_buffer_sizes() const {
    return packed_input_buffer_sizes_;
  }

  absl::Span<int64_t const> packed_output_buffer_sizes() const {
    return packed_output_buffer_sizes_;
  }

  absl::Span<int64_t const> input_buffer_preferred_alignments() const {
    return input_buffer_prefered_alignments_;
  }

  absl::Span<int64_t const> output_buffer_preferred_alignments() const {
    return output_buffer_prefered_alignments_;
  }

  absl::Span<int64_t const> input_buffer_abi_alignments() const {
    return input_buffer_abi_alignments_;
  }

  absl::Span<int64_t const> output_buffer_abi_alignments() const {
    return output_buffer_abi_alignments_;
  }

  int64_t temp_buffer_size() const { return temp_buffer_size_; }

  int64_t temp_buffer_alignment() const { return temp_buffer_alignment_; }

  const absl::flat_hash_map<int64_t, Node*>& continuation_points() const {
    return continuation_points_;
  }

 private:
  static absl::StatusOr<JittedFunctionBase> BuildInternal(
      FunctionBase* function, JitBuilderContext& jit_context,
      bool build_packed_wrapper);

  // The XLS FunctionBase this jitted function implements.
  FunctionBase* function_base_;

  // Name and function pointer for the jitted function which accepts/produces
  // arguments/results in LLVM native format.
  std::string function_name_;
  JitFunctionType function_;

  // Name and function pointer for the jitted function which accepts/produces
  // arguments/results in a packed format. Only exists for JITted
  // xls::Functions, not procs.
  std::optional<std::string> packed_function_name_;
  std::optional<JitFunctionType> packed_function_;

  // Sizes of the inputs/outputs in native LLVM format for `function_base`.
  std::vector<int64_t> input_buffer_sizes_;
  std::vector<int64_t> output_buffer_sizes_;

  // alignment preferences of each input/output buffer.
  std::vector<int64_t> input_buffer_prefered_alignments_;
  std::vector<int64_t> output_buffer_prefered_alignments_;

  // alignment ABI requirements of each input/output buffer.
  std::vector<int64_t> input_buffer_abi_alignments_;
  std::vector<int64_t> output_buffer_abi_alignments_;

  // Sizes of the inputs/outputs in packed format for `function_base`.
  std::vector<int64_t> packed_input_buffer_sizes_;
  std::vector<int64_t> packed_output_buffer_sizes_;

  // Size of the temporary buffer required by `function`.
  int64_t temp_buffer_size_ = -1;
  // Alignment of the temporary buffer required by `function`
  int64_t temp_buffer_alignment_ = -1;

  // Map from the continuation point return value to the corresponding node at
  // which execution was interrupted.
  absl::flat_hash_map<int64_t, Node*> continuation_points_;
};

}  // namespace xls

#endif  // XLS_JIT_FUNCTION_BASE_JIT_H_
