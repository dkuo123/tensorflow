#include <algorithm>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/vertex_templates.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <popops/DynamicSlice.hpp>
#include <popops/Pad.hpp>
#include <poputil/TileMapping.hpp>

namespace xla {
namespace poplarplugin {
namespace {
bool AreAllDimensionsConstant(const HloDynamicIndexInstruction* inst) {
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    if (!IsScalarIntegerConstant(inst->operand(i))) {
      return false;
    }
  }
  return true;
}

StatusOr<poplar::program::Program> ConstSliceUpdate(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor input = inputs[0][0];

  TF_ASSIGN_OR_RETURN(poplar::Tensor update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  std::vector<std::size_t> begin;
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    TF_ASSIGN_OR_RETURN(int64 index, LiteralScalarToNativeType<int64>(
                                         inst->operand(i)->literal()));
    begin.push_back(index);
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid slice start.");
  }

  std::vector<std::size_t> end = begin;
  for (unsigned int i = 0; i < end.size(); i++) {
    end[i] += update.dim(i);
  }
  poplar::Tensor slice = input.slice(begin, end);
  seq.add(poplar::program::Copy(update, slice));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, input));

  return seq;
}

StatusOr<poplar::program::Program> DynamicSliceUpdate(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor input = inputs[0][0];

  TF_ASSIGN_OR_RETURN(poplar::Tensor update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  TF_ASSIGN_OR_RETURN(poplar::Tensor indices,
                      FindInstructionInput(tensor_map, res, inst, 2, seq));

  auto first_index = inst->first_index_operand_number();

  bool multiple_indices = (indices.rank() == 0);

  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    poplar::Tensor t;
    if (multiple_indices) {
      TF_ASSIGN_OR_RETURN(
          t, FindInstructionInput(tensor_map, res, inst, first_index + d, seq));
      t = t.reshape({1});
    } else {
      t = indices.index({d}).reshape({1});
    }

    auto type = t.elementType();
    if (type == poplar::INT) {
      t = t.reinterpret(poplar::UNSIGNED_INT);
    }

    bool same_shape = inst->shape().dimensions(d) == update.shape()[d];
    unsigned int index;
    bool zero_index = t.getConstantValue(&index) && (index == 0);

    if (!(same_shape && zero_index)) {
      if (slice_dims.size() == 0) {
        slice_indices = t;
      } else {
        slice_indices = poplar::concat(slice_indices, t);
      }
      slice_dims.push_back(d);
      slice_sizes.push_back(update.shape()[d]);
    }
  }

  if (slice_dims.size() > 0) {
    popops::dynamicUpdate(graph, input, update, slice_indices, slice_dims,
                          slice_sizes, seq, GetDebugName(inst));
  } else {
    seq.add(poplar::program::Copy(update, input));
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, input));

  return seq;
}

StatusOr<poplar::program::Program> ConstSlice(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(poplar::Tensor input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  std::vector<std::size_t> begin;
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    TF_ASSIGN_OR_RETURN(int64 index, LiteralScalarToNativeType<int64>(
                                         inst->operand(i)->literal()));
    begin.push_back(index);
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid slice start.");
  }

  std::vector<std::size_t> end = begin;
  for (unsigned int i = 0; i < end.size(); i++) {
    end[i] += output_shape.dimensions(i);
  }

  poplar::Tensor slice = input.slice(begin, end);
  poplar::Tensor out = graph.clone(slice, GetDebugName(inst));

  seq.add(poplar::program::Copy(slice, out));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> DynamicSlice(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(poplar::Tensor input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  TF_ASSIGN_OR_RETURN(poplar::Tensor indices,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  auto first_index = inst->first_index_operand_number();

  bool multiple_indices = (indices.rank() == 0);

  auto& inst_slice_sizes = inst->dynamic_slice_sizes();
  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    poplar::Tensor t;
    if (multiple_indices) {
      TF_ASSIGN_OR_RETURN(
          t, FindInstructionInput(tensor_map, res, inst, first_index + d, seq));
      t = t.reshape({1});
    } else {
      t = indices.index({d}).reshape({1});
    }

    auto type = t.elementType();
    if (type == poplar::INT) {
      t = t.reinterpret(poplar::UNSIGNED_INT);
    }

    bool same_shape = inst_slice_sizes[d] == input.shape()[d];
    unsigned int index;

    bool zero_index = t.getConstantValue(&index) && (index == 0);

    if (!(same_shape && zero_index)) {
      if (slice_dims.size() == 0) {
        slice_indices = t;
      } else {
        slice_indices = poplar::concat(slice_indices, t, 0);
      }
      slice_dims.push_back(d);
      slice_sizes.push_back(inst_slice_sizes[d]);
    }
  }

  // Add the dynamic slice operations to `seq`. This automatically
  // creates the required compute set.
  poplar::Tensor out;

  if (slice_dims.size() > 0) {
    out = popops::dynamicSlice(graph, input, slice_indices, slice_dims,
                               slice_sizes, seq, GetDebugName(inst));
  } else {
    poplar::Tensor copy = graph.clone(input);
    seq.add(poplar::program::Copy(input, copy));
    out = copy;
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}
}  // namespace

StatusOr<poplar::program::Program> CreateDynamicSliceUpdateOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  auto* dynamic_inst = Cast<HloDynamicIndexInstruction>(inst);
  // See if we know the slice dimensions at the compile time.
  if (AreAllDimensionsConstant(dynamic_inst)) {
    VLOG(1) << "Processing " << inst->name() << " as a const slice update.";
    return ConstSliceUpdate(res, dynamic_inst, output_shape, tensor_map);
  } else {
    return DynamicSliceUpdate(res, dynamic_inst, output_shape, tensor_map);
  }
}

StatusOr<poplar::program::Program> CreateDynamicSliceOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  auto* dynamic_inst = Cast<HloDynamicIndexInstruction>(inst);
  // See if we know the slice dimensions at the compile time.
  if (AreAllDimensionsConstant(dynamic_inst)) {
    VLOG(1) << "Processing " << inst->name() << " as a const slice.";
    return ConstSlice(res, dynamic_inst, output_shape, tensor_map);
  } else {
    return DynamicSlice(res, dynamic_inst, output_shape, tensor_map);
  }
}

StatusOr<poplar::program::Program> CreateWideConstant(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root =
      inst->fused_instructions_computation()->root_instruction();

  const HloInstruction* constant = root->operand(0);
  const Literal& constant_literal = constant->literal();

  TensorSource src = std::make_pair(inst, 0);
  poplar::Tensor out;

  // For wide constants, check if they have an allocation target, if so then
  // allocate the wide constant with that target, otherwise allocate the scalar
  // constant and broadcast it.
  if (HasTensorAllocationTarget(src, res)) {
    // Unfortunately Literals are quite limited, and to create a literal of a
    // certain shape with the same value, we create a flat literal, repeatedly
    // memcpy the value and then reshape the literal into the desired shape.
    auto flat_shape = ShapeUtil::MakeShape(
        output_shape.element_type(), {ShapeUtil::ElementsIn(output_shape)});
    auto flat_literal = Literal(flat_shape);
    char* dest_data = static_cast<char*>(flat_literal.untyped_data());
    const char* source_data =
        static_cast<const char*>(constant_literal.untyped_data());
    const int64 primitive_size =
        ShapeUtil::ByteSizeOfPrimitiveType(output_shape.element_type());

    ShapeUtil::ForEachIndex(
        flat_shape, [&](absl::Span<const int64> output_index) {
          CHECK_EQ(output_index.size(), 1);
          memcpy(dest_data + primitive_size * output_index[0], source_data,
                 primitive_size);
          return true;
        });

    TF_ASSIGN_OR_RETURN(auto literal,
                        flat_literal.Reshape(output_shape.dimensions()));

    TF_ASSIGN_OR_RETURN(out, AddConstantTensor(graph, src, output_shape,
                                               literal, res, tensor_map));
  } else {
    TF_ASSIGN_OR_RETURN(
        out,
        AddConstantTensor(graph, std::make_pair(constant, 0), constant->shape(),
                          constant_literal, res, tensor_map));
    TF_ASSIGN_OR_RETURN(out, BroadcastTensor(out, output_shape, {}));
  }
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateZeroPadOp(CompilerResources& res,
                                                   const HloInstruction* inst,
                                                   const xla::Shape& output,
                                                   TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root =
      inst->fused_instructions_computation()->root_instruction();
  const PaddingConfig& cfg(root->padding_config());
  TF_ASSIGN_OR_RETURN(poplar::Tensor out,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  std::vector<std::ptrdiff_t> paddingLower;
  std::vector<std::ptrdiff_t> paddingUpper;
  for (auto& d : cfg.dimensions()) {
    paddingLower.push_back(d.edge_padding_low());
    paddingUpper.push_back(d.edge_padding_high());
  }
  out = popops::pad(graph, out, paddingLower, paddingUpper);

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));
  return seq;
}

}  // namespace poplarplugin
}  // namespace xla