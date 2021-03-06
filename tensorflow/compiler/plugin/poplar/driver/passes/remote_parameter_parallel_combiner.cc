/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/passes/remote_parameter_parallel_combiner.h"

#include <algorithm>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/remote_parameter.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_reachability.h"
#include "tensorflow/compiler/xla/service/hlo_value.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/str_util.h"

namespace xla {
namespace poplarplugin {

namespace {

std::vector<HloInstruction*> CombineOperands(
    const std::vector<HloInstruction*>& to_combine) {
  std::vector<HloInstruction*> operands;

  const auto* first_inst = to_combine.front();
  if (IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(first_inst)) {
    for (const auto* inst : to_combine) {
      operands.insert(operands.end(), inst->operands().cbegin(),
                      inst->operands().cend());
    }
  } else if (IsPoplarInstruction(PoplarOp::RemoteParameterStore)(first_inst)) {
    std::vector<HloInstruction*> remote_buffers;
    std::vector<HloInstruction*> values_to_store;
    for (const auto* inst : to_combine) {
      const auto* store_inst = Cast<HloRemoteParameterStore>(inst);
      remote_buffers.insert(remote_buffers.end(),
                            store_inst->RemoteBuffers().cbegin(),
                            store_inst->RemoteBuffers().cend());
      values_to_store.insert(values_to_store.end(),
                             store_inst->ValuesToStore().cbegin(),
                             store_inst->ValuesToStore().cend());
    }

    // The new list of operands has all the remote buffers first, then all the
    // corresponding values to store.
    operands.insert(operands.end(), remote_buffers.cbegin(),
                    remote_buffers.cend());
    operands.insert(operands.end(), values_to_store.cbegin(),
                    values_to_store.cend());
  } else {
    LOG(FATAL) << "Unexpected instruction: " << first_inst->ToString();
  }

  return operands;
}

std::vector<uint64> CombineReplicationFactors(
    const std::vector<HloInstruction*>& to_combine) {
  std::vector<uint64> replication_factors(to_combine.size());
  absl::c_transform(
      to_combine, replication_factors.begin(), [](const HloInstruction* inst) {
        if (IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(inst)) {
          return Cast<HloRemoteParameterLoad>(inst)->GetReplicationFactor(0);
        } else if (IsPoplarInstruction(PoplarOp::RemoteParameterStore)(inst)) {
          return Cast<HloRemoteParameterStore>(inst)->GetReplicationFactor(0);
        } else {
          LOG(FATAL) << "Unexpected instruction: " << inst->ToString();
        }
      });
  return replication_factors;
}

StatusOr<HloInstruction*> Combine(
    const std::vector<HloInstruction*>& to_combine) {
  const auto operands = CombineOperands(to_combine);
  const auto replication_factors = CombineReplicationFactors(to_combine);

  auto* first_inst = to_combine.front();
  HloComputation* comp = first_inst->parent();

  HloInstruction* new_inst = nullptr;
  if (IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(first_inst)) {
    new_inst = comp->AddInstruction(
        CreateHloRemoteParameterLoad(operands, replication_factors));
  } else if (IsPoplarInstruction(PoplarOp::RemoteParameterStore)(first_inst)) {
    new_inst = comp->AddInstruction(
        CreateHloRemoteParameterStore(operands, replication_factors));
    CHECK(absl::c_all_of(to_combine, IsLoweredInplace));
  } else {
    return FailedPrecondition("Unexpected instruction: %s",
                              first_inst->ToString().c_str());
  }
  first_inst->SetupDerivedInstruction(new_inst);
  new_inst->set_raw_backend_config_string(
      first_inst->raw_backend_config_string());
  return new_inst;
}

StatusOr<HloInstruction*> CombineAndReplace(
    const std::vector<HloInstruction*>& to_combine,
    TensorAllocationMap& allocation_map) {
  CHECK_GE(to_combine.size(), 2);
  HloComputation* comp = to_combine.front()->parent();

  // Combine the shapes into a tuple.
  std::vector<Shape> shapes(to_combine.size());
  absl::c_transform(to_combine, shapes.begin(),
                    [](HloInstruction* inst) { return inst->shape(); });
  const auto shape = ShapeUtil::MakeTupleShape(shapes);

  // Add the new instruction.
  TF_ASSIGN_OR_RETURN(HloInstruction * new_inst, Combine(to_combine));

  // Combine the sharding information into a tuple.
  std::vector<HloSharding> shardings;
  for (const auto* inst : to_combine) {
    shardings.push_back(inst->sharding());
  }
  new_inst->set_sharding(HloSharding::Tuple(shape, shardings));

  for (std::size_t i = 0; i < to_combine.size(); ++i) {
    auto* inst = to_combine[i];

    // Add an in-place GTE to unpack the new_inst result.
    auto* gte = comp->AddInstruction(
        HloInstruction::CreateGetTupleElement(inst->shape(), new_inst, i));
    MakeUsedInplace(gte);

    // Update tensor allocation info. Need to handle two cases:
    // 1) If this instruction was the source of an allocation target.
    auto itr = allocation_map.find(TensorLocation(inst, 0));
    if (itr != allocation_map.end()) {
      auto inserted = allocation_map.emplace(TensorLocation(new_inst, i),
                                             std::move(itr->second));
      // The new instruction should not be in the map already.
      CHECK(inserted.second);

      // Prepend the GTE to the backward tensor transformation path.
      auto& new_backward_path = inserted.first->second.backward_path;
      new_backward_path.insert(new_backward_path.begin(), gte);

      // Erase the old entry (with a now moved-from value).
      allocation_map.erase(itr);
    }

    // 2) If this instruction was the layout of an allocation target.
    for (auto& e : allocation_map) {
      TensorTarget& target = e.second;
      if (target.layout == inst) {
        target.layout = new_inst;
        CHECK(target.layout_output_idx.has_value());
        CHECK_EQ(*target.layout_output_idx, 0);
        target.layout_output_idx = i;
      }
    }

    // Replace the old inst.
    TF_RETURN_IF_ERROR(new_inst->CopyAllControlDepsFrom(inst));
    TF_RETURN_IF_ERROR(inst->DropAllControlDeps());
    TF_RETURN_IF_ERROR(inst->ReplaceAllUsesWith(gte));
    TF_RETURN_IF_ERROR(comp->RemoveInstruction(inst));
  }

  return new_inst;
}

bool IndependentlySchedulable(const std::vector<HloInstruction*>& instructions,
                              const HloReachabilityMap& reachability_map) {
  // Quadratic complexity in the number of shards; shouldn't be too bad.
  for (const auto* a : instructions) {
    for (const auto* b : instructions) {
      if (a != b && reachability_map.IsReachable(a, b)) {
        return false;
      }
    }
  }

  return true;
}

struct DecreasingSizeComparator {
  bool operator()(const HloInstruction* a, const HloInstruction* b) const {
    const auto a_size = ShapeUtil::ByteSizeOf(a->shape(), 1);
    const auto b_size = ShapeUtil::ByteSizeOf(b->shape(), 1);
    if (a_size != b_size) {
      return a_size < b_size;
    }

    // If the size is the same, order by parameter index.
    const int64 a_index = a->operand(0)->parameter_number();
    const int64 b_index = b->operand(0)->parameter_number();
    if (a_index != b_index) {
      return a_index > b_index;
    }

    // Everything else equal, defer to an arbitrary but deterministic order.
    return HloPtrComparator()(a, b);
  }
};

using DecreasingSizeQueue =
    std::priority_queue<HloInstruction*, std::vector<HloInstruction*>,
                        DecreasingSizeComparator>;

StatusOr<std::vector<HloInstruction*>> CombineFromDifferentShards(
    HloComputation* comp, std::map<int64, DecreasingSizeQueue> shard_queues,
    TensorAllocationMap& allocation_map) {
  std::vector<HloInstruction*> combined;

  while (true) {
    std::vector<HloInstruction*> to_combine;

    // Pop the largest one from each shard.
    for (auto& shard_queue : shard_queues) {
      auto& queue = shard_queue.second;
      if (!queue.empty()) {
        to_combine.push_back(queue.top());
        queue.pop();
      }
    }

    if (to_combine.size() < 2) {
      break;
    }

    // We must build a new reachability map because it does not support updates
    // to reflect the changes made by the combinations.
    const auto reachability_map = HloReachabilityMap::Build(comp);

    // We expect that the instructions in the different shards are not
    // dependent on each other, and hence can be combined safely. If this is
    // not the case, we just bail out of this attempt and try the next.
    if (!IndependentlySchedulable(to_combine, *reachability_map)) {
      VLOG(2) << "Skipping combination because of dependencies";
      continue;
    }

    TF_ASSIGN_OR_RETURN(auto* combined_inst,
                        CombineAndReplace(to_combine, allocation_map));

    combined.push_back(combined_inst);
  }

  // Return the instructions in order from smallest to largest. When trying to
  // schedule them in this order later this can help liveness, since we load the
  // largest parameters last when other tensors (like gradients for already
  // updated weights) might not be alive anymore.
  absl::c_reverse(combined);

  return combined;
}

Status ScheduleAllUsersBefore(HloInstruction* inst, HloInstruction* successor,
                              HloReachabilityMap& reachability_map) {
  for (auto* user : inst->users()) {
    if (!reachability_map.IsReachable(successor, user)) {
      TF_RETURN_IF_ERROR(user->AddControlDependencyTo(successor));
      reachability_map.UpdateReachabilityThroughInstruction(successor);
      TF_RETURN_IF_ERROR(
          ScheduleAllUsersBefore(user, successor, reachability_map));
    }
  }

  return Status::OK();
}

Status AddSchedulingConstraints(
    HloComputation* comp, const std::vector<HloInstruction*>& combined_loads,
    const std::vector<HloInstruction*>& combined_stores) {
  if (combined_loads.size() != combined_stores.size()) {
    // They're not matching up, bail out.
    return Status::OK();
  }

  auto reachability_map = HloReachabilityMap::Build(comp);

  for (std::size_t i = 1; i < combined_loads.size(); ++i) {
    auto* load = combined_loads[i];

    // To minimize liveness we aim towards having the least amount of overlap.
    // So first we try to schedule load[i] after store[i - 1], and if this is
    // not possible, we try to schedule it after store[i - 2] and so forth. A
    // typical reason why the first single-delay attempt might fail is when
    // using optimizers that require two offloaded parameters for each weight
    // update (like LAMB/ADAM that require both the first and second moments).
    for (std::size_t delay = 1; delay <= i; ++delay) {
      auto* prev_load = combined_loads[i - delay];

      // To minimze liveness, we also try to schedule all users of the previous
      // load before the current load. This attempts to ensure that the actual
      // weight update is pushed as early as possible in the schedule.
      TF_RETURN_IF_ERROR(
          ScheduleAllUsersBefore(prev_load, load, *reachability_map));

      auto* prev_store = combined_stores[i - delay];

      // If we can successfully schedule the previous store before this load,
      // we are satisifed with the scheduling constraints for this load and
      // break out to the next one.
      if (!reachability_map->IsReachable(load, prev_store)) {
        TF_RETURN_IF_ERROR(prev_store->AddControlDependencyTo(load));
        reachability_map->UpdateReachabilityThroughInstruction(load);
        break;
      }
    }
  }

  return Status::OK();
}

}  // namespace

StatusOr<bool> RemoteParameterParallelCombiner::RunOnComputation(
    HloComputation* comp) {
  std::map<int64, DecreasingSizeQueue> shard_loads;
  std::map<int64, DecreasingSizeQueue> shard_stores;

  for (auto* inst : comp->MakeInstructionPostOrder()) {
    if (auto shard = inst->sharding_unique_device()) {
      if (IsPoplarInstruction(RemoteParameterLoad)(inst)) {
        shard_loads[*shard].push(Cast<HloRemoteParameterLoad>(inst));
      } else if (IsPoplarInstruction(RemoteParameterStore)(inst)) {
        shard_stores[*shard].push(Cast<HloRemoteParameterStore>(inst));
      }
    }
  }

  TF_ASSIGN_OR_RETURN(const auto combined_loads,
                      CombineFromDifferentShards(comp, std::move(shard_loads),
                                                 allocation_map_));

  TF_ASSIGN_OR_RETURN(const auto combined_stores,
                      CombineFromDifferentShards(comp, std::move(shard_stores),
                                                 allocation_map_));

  // Try to help the scheduler a bit by adding some constraints.
  TF_RETURN_IF_ERROR(
      AddSchedulingConstraints(comp, combined_loads, combined_stores));

  return !combined_loads.empty() || !combined_stores.empty();
}

StatusOr<bool> RemoteParameterParallelCombiner::Run(HloModule* module) {
  VLOG(2) << "Before RemoteParameterParallelCombiner:";
  XLA_VLOG_LINES(2, module->ToString());

  bool changed = false;

  // Run it for all resource updates.
  for (auto* comp : module->MakeComputationPostOrder()) {
    if (IsPopOpsFusion(comp)) {
      continue;
    }

    for (auto* inst : comp->MakeInstructionPostOrder()) {
      if (IsResourceUpdate(inst)) {
        TF_ASSIGN_OR_RETURN(const bool computation_changed,
                            RunOnComputation(inst->to_apply()));
        changed |= computation_changed;
      }
    }
  }

  if (changed) {
    VLOG(2) << "After RemoteParameterParallelCombiner:";
    XLA_VLOG_LINES(2, module->ToString());
  } else {
    VLOG(2) << "No changes were made.";
  }
  return changed;
}

}  // namespace poplarplugin
}  // namespace xla
