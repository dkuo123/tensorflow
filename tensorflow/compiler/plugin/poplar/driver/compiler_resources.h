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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_RESOURCES_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_RESOURCES_H_

#include <memory>
#include <poplar/Graph.hpp>
#include <poplar/OptionFlags.hpp>
#include <poplin/Convolution.hpp>
#include <poplin/MatMul.hpp>
#include <popops/DynamicSlice.hpp>
#include <poprand/RandomGen.hpp>
#include <poputil/GraphFunction.hpp>
#include <stack>
#include <string>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_information.h"
#include "tensorflow/compiler/plugin/poplar/driver/config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/conv_graph_caching.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/convolution_classifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/execution_counter_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/generic_graph_caching.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/mapping_helper.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/subcomputation_graph_caching.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/verified_streams_indices.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/deferred_visitor.h"

namespace xla {
class HloInstruction;
class CallGraph;

namespace poplarplugin {

// This structure contains additional information required to lower the graph
// from an XLA graph to a poplar graph.
struct CompilerResources {
  std::unique_ptr<poplar::Graph> main_graph;

  std::vector<poplar::Graph> shard_graphs;

  std::vector<unsigned> shard_to_ipu_id;

  absl::flat_hash_map<const HloInstruction*, const popops::SlicePlan*>
      slice_plan_mappings;

  std::list<popops::SlicePlan> slice_plans;
  absl::flat_hash_set<const popops::SlicePlan*> used_slice_plan;

  CompilerAnnotations annotations;

  const CompilerInformation information;

  poplin::PlanningCache convolution_cache;

  poplin::matmul::PlanningCache matmul_cache;

  poplin::matmul::PlanningCache dot_cache;

  const IpuOptions::FloatingPointBehaviour global_floating_point_behaviour;

  const poplar::OptionFlags default_conv_options;

  const poplar::OptionFlags default_matmul_options;

  const poplar::OptionFlags default_pooling_options;

  bool use_verified_transfers;

  bool clear_matmul_pass_type;

  bool disable_graph_convolution_caching;

  bool disable_graph_outlining;

  /* The global number of replicas that we are compiling for. */
  uint32 replication_factor;

  /* The local number of replicas owned by this process. This is the number of
   * replicas that we are responsible for at run-time in this process. This is
   * only different from the `replication_factor` when using multi-replica
   * distribution with the Poplar "runtime replica subset" feature. */
  uint32 local_replication_factor;

  bool merge_infeed_io_copies;

  bool always_rearrange_copies_on_host;

  TensorMaps tensor_maps;

  LinearMapperState linear_mapping_state;

  conv_graph_caching::ConvolutionGraphCache conv_graph_cache;

  conv_graph_caching::BwdWeightGraphCache bwd_weight_graph_cache;

  generic_graph_caching::GenericGraphCache graph_cache;

  subcomputation_graph_caching::SubcomputationGraphCache subcomputation_cache;

  poplar::program::Sequence preamble_sequence;

  std::stack<std::vector<poplar::program::Sequence>>
      gradient_accumulation_zeroing_sequences;

  std::stack<std::vector<poplar::program::Sequence>>
      pipelining_write_undef_sequences;

  std::stack<DeferredAllocations> deferred_allocation_scopes;

  std::stack<ExecutionCounters*> execution_counter_scopes;

  std::string scheduler_selection;

  bool recomputation_enabled;

  bool use_stable_norm_statistics;

  bool remote_memory_supported;

  poplar::OptionFlags gcl_options;

  int64 triangular_solve_expander_block_size;

  std::unique_ptr<CallGraph> module_call_graph;

  absl::flat_hash_map<std::string, poplar::RemoteBuffer> remote_buffers;

  VerifiedStreamsIndices streams_indices;

  bool enable_experimental_remote_buffer_embedding;

  bool enable_fast_math;

  absl::flat_hash_set<std::string> custom_codelets_in_graph;

  CompilerResources(
      HloModule* module, const CompilerInformation& information,
      const poplar::OptionFlags& conv_options,
      const poplar::OptionFlags& matmul_options,
      const poplar::OptionFlags& pooling_options, bool verified_transfers,
      bool clear_matmul_pass_type, bool disable_graph_convolution_caching,
      bool disable_graph_outlining, bool merge_infeed_io_copies,
      uint32 replication_factor, uint32 local_replication_factor,
      const IpuOptions::FloatingPointBehaviour& floating_point_behaviour,
      bool always_rearrange_copies_on_host,
      const std::string& scheduler_selection, bool recomputation_enabled,
      bool use_stable_norm_statistics, bool remote_memory_supported,
      const poplar::OptionFlags& gcl_options,
      int64 triangular_solve_expander_block_size,
      bool enable_experimental_remote_buffer_embedding, bool enable_fast_math)
      : annotations(module),
        information(information),
        global_floating_point_behaviour(floating_point_behaviour),
        default_conv_options(conv_options),
        default_matmul_options(matmul_options),
        default_pooling_options(pooling_options),
        use_verified_transfers(verified_transfers),
        clear_matmul_pass_type(clear_matmul_pass_type),
        disable_graph_convolution_caching(disable_graph_convolution_caching),
        disable_graph_outlining(disable_graph_outlining),
        replication_factor(replication_factor),
        local_replication_factor(local_replication_factor),
        merge_infeed_io_copies(merge_infeed_io_copies),
        always_rearrange_copies_on_host(always_rearrange_copies_on_host),
        scheduler_selection(scheduler_selection),
        recomputation_enabled(recomputation_enabled),
        use_stable_norm_statistics(use_stable_norm_statistics),
        remote_memory_supported(remote_memory_supported),
        gcl_options(gcl_options),
        triangular_solve_expander_block_size(
            triangular_solve_expander_block_size),
        enable_experimental_remote_buffer_embedding(
            enable_experimental_remote_buffer_embedding),
        enable_fast_math(enable_fast_math) {}

  static std::unique_ptr<CompilerResources> CreateTestDefault(
      HloModule* module,
      const CompilerInformation& information = CompilerInformation()) {
    return absl::make_unique<CompilerResources>(
        module, information,
        /*conv_options=*/poplar::OptionFlags(),
        /*matmul_options=*/poplar::OptionFlags(),
        /*pooling_options=*/poplar::OptionFlags(), /*verified_transfers=*/false,
        /*clear_matmul_pass_type=*/false,
        /*disable_graph_convolution_caching=*/false,
        /*disable_graph_outlining=*/false, /*merge_infeed_io_copies=*/false,
        /*replication_factor=*/1, /*local_replication_factor=*/1,
        /*floating_point_behaviour=*/IpuOptions::FloatingPointBehaviour(),
        /*always_rearrange_copies_on_host=*/false, /*scheduler_selection=*/"",
        /*recomputation_enabled=*/false, /*use_stable_norm_statistics=*/false,
        /*remote_memory_supported=*/false,
        /*gcl_options=*/poplar::OptionFlags(),
        /*triangular_solve_expander_block_size=*/0,
        /*enable_experimental_remote_buffer_embedding=*/false,
        /*enable_fast_math=*/false);
  }
};

}  // namespace poplarplugin
}  // namespace xla

#endif
