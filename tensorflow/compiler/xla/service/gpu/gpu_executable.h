/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "tensorflow/compiler/xla/service/buffer_assignment.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/gpu/buffer_allocations.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_types.h"
#include "tensorflow/compiler/xla/service/gpu/stream_assignment.h"
#include "tensorflow/compiler/xla/service/gpu/thunk.h"
#include "tensorflow/compiler/xla/service/gpu/thunk_schedule.h"
#include "tensorflow/compiler/xla/service/hlo_dataflow_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_execution_profile.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"

namespace xla {
namespace gpu {

// GPU-targeting implementation of the XLA Executable interface.
//
// Launches the given GPU kernel via the StreamExecutor.
//
// This is an immutable data type after initialization, and thus thread safe.
class GpuExecutable : public Executable {
  struct BefBufferDeleter {
    void operator()(uint8_t* ptr) const;
    size_t size;
  };

 public:
  typedef std::unique_ptr<const ThunkSchedule> OwnedThunkSchedule;
  typedef std::unique_ptr<uint8_t, BefBufferDeleter> OwnedBefBuffer;

  struct ConstantInfo {
    std::string symbol_name;
    std::vector<uint8> content;
    int allocation_index = -1;
  };

  struct OutputInfo {
    // Corresponding allocation index.
    int allocation_index;

    // Output is passed-through from a parameter.
    bool passthrough = false;

    // Whether this output is hinted to alias a parameter (BufferAllocation*
    // would indicate the aliased parameter), and what kind of alias it is.
    absl::optional<HloInputOutputAliasConfig::Alias> alias_config;
  };

  struct Params {
    std::string asm_text;
    std::vector<uint8> binary;
    GpuVersion gpu_version;
    // The GpuExecutable will either execute Thunks or a whole-program BEF
    // depending on which is supplied.
    absl::variant<OwnedThunkSchedule, OwnedBefBuffer> thunks_or_bef;
    std::vector<ConstantInfo> constants;
    absl::flat_hash_map<ShapeIndex, OutputInfo> output_info;
    std::string module_name;
    xla::Shape output_shape;
    std::vector<BufferAllocation> allocations;
    std::unique_ptr<BufferAssignmentProto> debug_buffer_assignment = nullptr;
    std::string verbose_buffer_assignment_string = "";
    std::unique_ptr<HloModule> debug_module = nullptr;
    size_t entry_computation_profile_index = 0;
    std::unique_ptr<HloProfilePrinterData> hlo_profile_printer_data = nullptr;
    std::unique_ptr<HloProfileIndexMap> hlo_profile_index_map = nullptr;
  };

  // We need to share ownership of hlo_module and assignment with profiler to
  // safely keep a reference to these objects during tracing period, thus they
  // are passed as shared pointers.
  explicit GpuExecutable(Params params);
  ~GpuExecutable() override;

  int64_t SizeOfGeneratedCodeInBytes() const override;

  // This should be called after set_ir_module_string.
  const string& ir_module_string() const { return ir_module_string_; }

  // This should be called before ExecuteOnStream.
  void set_ir_module_string(const string& ir_module_string) {
    ir_module_string_ = ir_module_string;
  }

  // Returns the compiled code for the computation. The compiled code is PTX in
  // Cuda and unused empty string in ROCm.
  const string& text() const { return text_; }

  // Returns the binary stored in this GpuExecutable. The binary is cubin in
  // Cuda, and HSA code object in ROCm. It may be empty, in which case
  // compilation is left up to the GPU driver.
  const std::vector<uint8>& binary() const { return binary_; }

  // ExecuteAsyncOnStream will fail if the compute capability of the stream
  // doesn't match the compute capability passed to this object's constructor.
  StatusOr<ExecutionOutput> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      std::vector<ExecutionInput> arguments,
      HloExecutionProfile* hlo_execution_profile) override;

  StatusOr<ScopedShapedBuffer> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      absl::Span<const ShapedBuffer* const> arguments,
      HloExecutionProfile* hlo_execution_profile) override;

  using VariantArguments = absl::variant<absl::Span<const ShapedBuffer* const>,
                                         absl::Span<ExecutionInput>>;
  StatusOr<ExecutionOutput> ExecuteAsyncOnStreamImpl(
      const ServiceExecutableRunOptions* run_options,
      VariantArguments arguments);

  absl::Span<const BufferAllocation> GetAllocations() const {
    return allocations_;
  }

  const std::vector<ConstantInfo>& constants() const { return constants_; }

 private:
  // If `block_host_until_done` is false, execution will not block the host
  // until the kernels have completed. This is used as an optimization for
  // clients, such as Tensorflow, that use a single stream of execution for
  // computations, and allow host-side deallocation from the allocator before
  // GPU execution completes.
  Status ExecuteThunks(const ThunkSchedule& thunk_schedule,
                       const ServiceExecutableRunOptions* run_options,
                       const BufferAllocations& buffer_allocations,
                       bool block_host_until_done);

  using BufferAllocToDeviceMemoryMap =
      absl::flat_hash_map<BufferAllocation::Index, se::DeviceMemoryBase>;

  // Loads the PTX or CUBIN for this executable into `executor` and resolves the
  // globals corresponding to constant buffers.  Returns a map mapping buffer
  // allocation indices to GPU pointers.
  StatusOr<const BufferAllocToDeviceMemoryMap*> ResolveConstantGlobals(
      stream_executor::Stream* stream);

  // GpuExecutable check with either AMD's ISA version, or Nvidia's major minor
  // version for compute capability, depending on the hardware.
  Status CheckCompatibilityWithServiceExecutableRunOptions(
      const ServiceExecutableRunOptions* run_options);

  StatusOr<BufferAllocations> GenerateBufferAllocations(
      VariantArguments arguments,
      const GpuExecutable::BufferAllocToDeviceMemoryMap* globals,
      se::DeviceMemoryAllocator* const memory_allocator,
      se::StreamExecutor* executor);

  StatusOr<se::DeviceMemoryBase> BufferForAllocation(
      VariantArguments arguments,
      const GpuExecutable::BufferAllocToDeviceMemoryMap* globals,
      const BufferAllocation& allocation,
      se::DeviceMemoryAllocator* const memory_allocator, int device_ordinal,
      int64_t arg_idx);

  // The LLVM IR, in string format, of the unoptimized module generated for
  // this GpuExecutable. We save a string instead of an llvm::Module* because
  // leaving llvm::Module* in a singleton can cause the heap checker to emit
  // false positives.
  //
  // This string should be modified only before ExecuteOnStream.
  string ir_module_string_;

  // The compiled code for the computation.
  const string text_;

  // The GPU machine code for the computation, targeting GPUs at
  // compute_capability_.
  //
  // May be empty, in which case we leave compilation up to the GPU driver.
  const std::vector<uint8> binary_;

  // The GPU version for compute compatibility check.
  GpuVersion gpu_version_;

  // The thunks to be invoked by this GpuExecutable. They are generated by the
  // IrEmitter.
  absl::variant<OwnedThunkSchedule, OwnedBefBuffer> thunks_or_bef_;

  std::string module_name_;

  xla::Shape output_shape_;

  // Owns the buffer data at runtime. It provides information to allocate
  // memory for every output/temp buffers.
  const std::vector<BufferAllocation> allocations_;

  std::shared_ptr<BufferAssignmentProto> debug_buffer_assignment_;
  std::string verbose_buffer_assignment_string_;

  size_t entry_computation_profile_index_ = -1;

  // Cache of module handles and constant buffer allocation maps used by
  // `ResolveConstantGlobals`.
  tensorflow::mutex module_handle_mutex_;
  std::map<stream_executor::StreamExecutor*, se::ScopedModuleHandle>
      module_handles_ TF_GUARDED_BY(module_handle_mutex_);
  std::map<stream_executor::StreamExecutor*, BufferAllocToDeviceMemoryMap>
      module_globals_ TF_GUARDED_BY(module_handle_mutex_);

  std::vector<ConstantInfo> constants_;
  const absl::flat_hash_map<ShapeIndex, OutputInfo> output_info_;

  TF_DISALLOW_COPY_AND_ASSIGN(GpuExecutable);
};

StatusOr<absl::flat_hash_map<ShapeIndex, GpuExecutable::OutputInfo>>
GetOutputInfo(const HloModule& hlo_module, const BufferAssignment& assignment);

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_EXECUTABLE_H_
