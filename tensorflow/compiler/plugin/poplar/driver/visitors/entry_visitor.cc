/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/visitors/entry_visitor.h"

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"

namespace xla {
namespace poplarplugin {

Status EntryVisitor::HandleParameter(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  // Go through all the shapes for inst, don't allocate any tensors which are
  // marked as deferred.
  std::vector<Shape> shapes = FlattenedXlaShape(inst->shape());
  for (int64 i = 0; i < shapes.size(); i++) {
    if (CanDeferAllocation(inst, i)) {
      VLOG(1) << "Deferring allocation of " << inst->name() << " sub tensor "
              << i << ".";
      DeferAllocation(inst, i);
    } else {
      TF_RETURN_IF_ERROR(AllocateInput(inst, i, shapes[i]));
    }
  }
  return Status::OK();
}

StatusOr<poplar::Tensor> EntryVisitor::PostProcessParameterAllocation(
    const HloInstruction* inst, const int64 flat_tuple_index,
    poplar::Tensor tensor) {
  const auto& in_info = resources_.annotations.input_output_aliasing_map
                            .GetEntryInputInfos()[inst->parameter_number()];

  std::vector<Shape> module_shapes;

  const HloModule* module = inst->GetModule();
  const ComputationLayout layout = module->entry_computation_layout();
  if (layout.parameter_count() > inst->parameter_number()) {
    const Shape& mod_shape = layout.parameter_shape(inst->parameter_number());
    module_shapes = FlattenedXlaShape(mod_shape);
  }

  poplar::program::Sequence& stream_copy_seq =
      in_info.IsStreaming() ? sequence : host_to_device;

  poplar::program::Sequence& inter_ipu_copy_seq =
      in_info.IsStreaming() ? sequence : host_to_device_inter_ipu_copy;

  if (!UseSyntheticData()) {
    poplar::Graph& master_graph = GetMasterGraph(resources_);
    poplar::Tensor master_tensor = tensor;
    poplar::Tensor input_tensor = tensor;

    const auto replication_factor = resources_.replication_factor;
    if (HasReplicatedGraph(resources_)) {
      master_tensor = master_graph.getNonReplicatedTensor(master_tensor);
      if (replication_factor != master_tensor.dim(0)) {
        return xla::FailedPrecondition(
            "Unable to stream replicated tensor - replication count does not "
            "match (%llu vs %llu).",
            replication_factor, master_tensor.dim(0));
      }
      // For replicated graphs we copy from the host to IPU 0, then copy to the
      // other IPUs
      input_tensor = master_tensor.slice(0, 1);
    }

    auto fifo = master_graph.addHostToDeviceFIFO(
        GetInputCopyHandle(inst->parameter_number(), flat_tuple_index),
        input_tensor.elementType(), input_tensor.numElements());

    stream_copy_seq.add(poplar::program::Copy(
        fifo, input_tensor,
        !in_info.IsStreaming() || always_rearrange_copies_on_the_host));

    if (HasReplicatedGraph(resources_)) {
      inter_ipu_copy_seq.add(poplar::program::Copy(
          input_tensor.broadcast(replication_factor - 1, 0),
          master_tensor.slice(1, replication_factor)));
    }
  }

  if (!LayoutUtil::IsMonotonicWithDim0Major(
          module_shapes[flat_tuple_index].layout())) {
    // Host tensor needs to be host layout
    non_standard_parameter_layout.insert(inst);
    tensor = ConvertToDeviceLayout(module_shapes[flat_tuple_index], tensor);
  }

  // If a the input to the graph is a resource variable which does not change
  // a value, then add a clone/copy to make sure it does not get overwritten
  // between runs
  if (in_info.IsResourceNotModified()) {
    poplar::Tensor non_modified_tensor = tensor;
    poplar::Graph& graph =
        GetGraphWithOutputIndex(resources_, inst, flat_tuple_index);
    tensor = graph.clone(non_modified_tensor,
                         GetDebugName(inst) + ".resource_not_modified_clone");
    sequence.add(poplar::program::Copy(non_modified_tensor, tensor));
  }
  return tensor;
}

Status EntryVisitor::FinishVisit(HloInstruction* root) {
  VLOG(1) << "Processing FinishVisit";
  HloComputation* comp = root->parent();
  if (ShapeUtil::IsEmptyTuple(root->shape())) {
    VLOG(1) << "Root instruction shape is empty tuple";
    resources_.tensor_maps[comp->name()] = std::move(tensor_map);
    return Status::OK();
  }

  auto* layout = comp->parent()->mutable_entry_computation_layout();
  std::vector<Shape> shapes = FlattenedXlaShape(layout->result_shape());

  const auto& entry_outputs =
      resources_.annotations.input_output_aliasing_map.GetEntryOutputInfos();

  const uint64 num_outputs =
      root->shape().IsTuple() ? ShapeUtil::TupleElementCount(root->shape()) : 1;

  CHECK_EQ(num_outputs, entry_outputs.size());
  // Go through all the flat tensor outputs
  // *Reminder* We use depth-first flattening of nested tuples for inputs and
  // outputs
  uint64 from_tensor_index = 0;
  uint64 to_tensor_index = 0;
  // TODO see T5364
  auto out_tensors =
      FindExpandedInstructionOutputs(tensor_map, resources_, root, sequence);
  for (uint64 idx = 0; idx < entry_outputs.size(); idx++) {
    auto& out_info = entry_outputs[idx];
    poplar::program::Sequence& seq =
        out_info.IsStreaming() ? sequence : device_to_host;

    // Flatten the tuple tensor (if required) and iterate over all of them
    const auto sub_shape =
        root->shape().IsTuple()
            ? ShapeUtil::GetTupleElementShape(root->shape(), idx)
            : root->shape();
    to_tensor_index +=
        sub_shape.IsTuple() ? ShapeUtil::TupleElementCount(sub_shape) : 1;
    // all_outputs_flat_tensor_index is the global index into all the flattened
    // output tensors
    // current_output_flat_tensor_index is the local index into all the
    // flattened tensors for for output idx
    for (uint64 all_outputs_flat_tensor_index = from_tensor_index,
                current_output_flat_tensor_index = 0;
         all_outputs_flat_tensor_index < to_tensor_index;
         all_outputs_flat_tensor_index++, current_output_flat_tensor_index++) {
      if (out_info.IsResourceModified()) {
        // Get the mapped input and make sure they are the same tensor,
        // otherwise add a on device copy to make sure location of the resource
        // variable doesn't change between the runs
        // (the alternative is to reload the graph everytime)
        auto in_tensors = FindInstructionOutputs(
            tensor_map, comp->parameter_instruction(out_info.GetInputIndex()));
        if (in_tensors[current_output_flat_tensor_index] !=
            out_tensors[all_outputs_flat_tensor_index]) {
          sequence.add(poplar::program::Copy(
              out_tensors[all_outputs_flat_tensor_index],
              in_tensors[current_output_flat_tensor_index]));
        }
      }

      if (!UseSyntheticData()) {
        poplar::Tensor out =
            ConvertFromDeviceLayout(shapes[all_outputs_flat_tensor_index],
                                    out_tensors[all_outputs_flat_tensor_index]);

        poplar::Graph& master_graph = GetMasterGraph(resources_);

        if (HasReplicatedGraph(resources_)) {
          // For replicated outputs, we send only the first IPU's slice to the
          // host
          out = master_graph.getNonReplicatedTensor(out);
          out = out.slice(0, 1);
        }

        auto fifo = master_graph.addDeviceToHostFIFO(
            GetOutputCopyHandle(idx, current_output_flat_tensor_index),
            out.elementType(), out.numElements());

        seq.add(poplar::program::Copy(
            out, fifo,
            !out_info.IsStreaming() || always_rearrange_copies_on_the_host));
      }
    }
    from_tensor_index = to_tensor_index;
  }

  resources_.tensor_maps[comp->name()] = std::move(tensor_map);

  return Status::OK();
}

const std::set<const HloInstruction*>&
EntryVisitor::GetNonStandardParameterLayout() const {
  return non_standard_parameter_layout;
}

const poplar::program::Sequence EntryVisitor::GetHostToDevice() const {
  poplar::program::Sequence seq;
  seq.add(host_to_device);
  seq.add(host_to_device_inter_ipu_copy);
  return seq;
}
const poplar::program::Sequence EntryVisitor::GetDeviceToHost() const {
  return device_to_host;
}

}  // namespace poplarplugin
}  // namespace xla