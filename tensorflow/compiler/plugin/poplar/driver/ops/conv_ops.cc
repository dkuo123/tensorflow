#include <algorithm>

#include "absl/strings/str_cat.h"

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/conv_graph_caching.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/convolution_classifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/conv_poplar_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/conv_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/generic_graph_caching.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/ml_type_helper.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/vertex_templates.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/util/bcast.h"

#include <poplin/Convolution.hpp>
#include <popops/Cast.hpp>
#include <popops/ElementWise.hpp>
#include <popops/Reduce.hpp>
#include <popops/ScaledAdd.hpp>

using ::absl::StrCat;

namespace xla {
namespace poplarplugin {

// This function operates on the poplibs format weights (GOI...)
poplar::Tensor RemoveGroupsDimensionFromWeights(const poplin::ConvParams& p,
                                                const poplar::Tensor& t,
                                                bool flipped) {
  poplar::Tensor out = t;
  return out.reshapePartial(0, 2, {out.dim(0) * out.dim(1)});
}

// This function operates on the poplibs format weights (GOI...)
poplar::Tensor AddGroupsDimensionToWeights(const poplin::ConvParams& p,
                                           const poplar::Tensor& t,
                                           bool flipped) {
  poplar::Tensor out = t;

  unsigned int out_dim = flipped ? 1 : 0;
  unsigned int in_dim = 1 - out_dim;

  if (p.getNumConvGroups() == 1) {
    // Non-grouped case
    return out.reshapePartial(0, 0, {1});
  } else {
    unsigned int chan_div[2];
    chan_div[in_dim] = out.dim(in_dim) / p.getNumInputChansPerConvGroup();
    chan_div[out_dim] = out.dim(out_dim) / p.getNumOutputChansPerConvGroup();

    // OI... ->(GO)(GI)...
    out = out.reshapePartial(0, 2,
                             {chan_div[0], out.dim(0) / chan_div[0],
                              chan_div[1], out.dim(1) / chan_div[1]});

    // (GO)(GI)... -> (GG)OI...
    out = out.dimShufflePartial({2}, {1});

    // (GG)OI... -> GOI...
    return out.reshapePartial(0, 2, {out.dim(0) * out.dim(1)});
  }
}

StatusOr<poplar::program::Program> CreateConv2D(CompilerResources& res,
                                                const HloInstruction* inst,
                                                const xla::Shape& output_shape,
                                                TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence prog;

  // Find the input tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor in,
                      FindInstructionInput(tensor_map, res, inst, 0, prog));

  // Find the kernel tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor kernel,
                      FindInstructionInput(tensor_map, res, inst, 1, prog));

  TF_ASSIGN_OR_RETURN(poplin::ConvParams params,
                      GetConvolutionParameters(inst, 0, 1));

  in = ShuffleConvolutionInputToPoplar(inst, in);

  kernel = ShuffleConvolutionWeightsToPoplar(inst, kernel, false);

  kernel = AddGroupsDimensionToWeights(params, kernel, false);

  TF_ASSIGN_OR_RETURN(const MLType conv_type, GetMLType(inst));

  TF_ASSIGN_OR_RETURN(
      poplar::Tensor out,
      conv_graph_caching::DoCachedConvolution(
          graph, res, in, kernel, params, conv_type, false,
          GetSingleShardingDeviceId(inst), prog, GetDebugName(inst)));

  out = ShuffleConvolutionOutputToTensorflow(inst, out);

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return prog;
}

StatusOr<poplar::program::Program> Create2DConvWithReverse(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence prog;

  // Find the input tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor in,
                      FindInstructionInput(tensor_map, res, inst, 0, prog));

  // Find the kernel tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor kernel,
                      FindInstructionInput(tensor_map, res, inst, 1, prog));

  TF_ASSIGN_OR_RETURN(poplin::ConvParams params,
                      GetConvolutionParameters(inst, 0, 1));

  in = ShuffleConvolutionInputToPoplar(inst, in);

  kernel = ShuffleConvolutionWeightsToPoplar(inst, kernel, true);

  kernel = AddGroupsDimensionToWeights(params, kernel, true);

  TF_ASSIGN_OR_RETURN(const MLType conv_type, GetMLType(inst));

  TF_ASSIGN_OR_RETURN(
      poplar::Tensor out,
      conv_graph_caching::DoCachedConvolution(
          graph, res, in, kernel, params, conv_type, true,
          GetSingleShardingDeviceId(inst), prog, GetDebugName(inst)));

  out = ShuffleConvolutionOutputToTensorflow(inst, out);

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return prog;
}

StatusOr<poplar::program::Program> CreateDepthwiseBackpropFilter(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence prog;

  // Find the input tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor in,
                      FindInstructionInput(tensor_map, res, inst, 0, prog));

  // Find the kernel tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor kernel,
                      FindInstructionInput(tensor_map, res, inst, 1, prog));

  TF_ASSIGN_OR_RETURN(poplin::ConvParams params,
                      GetConvolutionParameters(inst, 0, 1));

  in = ShuffleConvolutionInputToPoplar(inst, in);

  // Move 'G' parts of the I to B (because B is the reducing dimension)
  unsigned n_g = params.getNumConvGroups();
  in = in.reshapePartial(0, 1, {n_g, in.dim(0) / n_g});
  in = in.dimShufflePartial({0}, {1});
  in = in.reshapePartial(1, 3, {in.dim(1) * in.dim(2)});

  kernel = ShuffleConvolutionWeightsToPoplar(inst, kernel, false);

  kernel = AddGroupsDimensionToWeights(params, kernel, false);

  TF_ASSIGN_OR_RETURN(const MLType conv_type, GetMLType(inst));

  TF_ASSIGN_OR_RETURN(
      poplar::Tensor out,
      conv_graph_caching::DoCachedConvolution(
          graph, res, in, kernel, params, conv_type, false,
          GetSingleShardingDeviceId(inst), prog, GetDebugName(inst)));

  // Move 'G' parts of the B back to I
  out = out.reshapePartial(1, 2, {n_g, out.dim(1) / n_g});
  out = out.dimShufflePartial({1}, {0});
  out = out.reshapePartial(0, 2, {out.dim(0) * out.dim(1)});

  out = ShuffleConvolutionOutputToTensorflow(inst, out);

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return prog;
}

StatusOr<poplar::program::Program> CreateConvScaledInplace(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  // Find the weights tensor
  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor arg_weights = inputs[0][0];

  // Find the input tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor arg_in,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  // Find the deltas tensor
  TF_ASSIGN_OR_RETURN(poplar::Tensor arg_deltas,
                      FindInstructionInput(tensor_map, res, inst, 2, seq));

  // Find the scale.
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor arg_scale,
      FindInstructionInput(tensor_map, res, inst, 3, seq, false));

  TF_ASSIGN_OR_RETURN(poplin::ConvParams params,
                      GetConvolutionParameters(inst, 1, 2));

  TF_ASSIGN_OR_RETURN(const MLType conv_type, GetMLType(inst));
  auto opts = GetConvolutionOptionsForType(res, conv_type);
  const ConvolutionDimensionNumbers& conv_dims = GetConvolutionDims(inst);

  // Get the root of the fusion - it indicates whether this is add or
  // subtract.
  const auto* root_inst =
      inst->fused_instructions_computation()->root_instruction();
  auto op_type = root_inst->opcode();

  const std::string debug_prefix = GetDebugName(inst);

  using namespace poputil::graphfn;
  auto func = [&graph, &res, params, opts, conv_dims, op_type, debug_prefix](
                  std::vector<poplar::Tensor>& args,
                  poplar::program::Sequence& prog) {
    poplar::Tensor weights = args[0];
    poplar::Tensor in = args[1];
    poplar::Tensor deltas = args[2];
    poplar::Tensor scale = args[3];

    weights = ShuffleConvolutionOutputToPoplar(conv_dims, weights);
    in = ShuffleConvolutionInputToPoplar(conv_dims, in);
    deltas = ShuffleConvolutionWeightsToPoplar(conv_dims, deltas, false);
    deltas = AddGroupsDimensionToWeights(params, deltas, false);

    auto c_out =
        poplin::convolution(graph, in, deltas, params, false, prog,
                            debug_prefix, opts, &res.convolution_cache);

    TF_CHECK_OK(ScaledInplaceConstantOrTensor(graph, weights, c_out, scale,
                                              prog, op_type, debug_prefix));

    args[0] = ShuffleConvolutionOutputToTensorflow(conv_dims, weights);
  };

  std::vector<poplar::Tensor> args = {arg_weights, arg_in, arg_deltas,
                                      arg_scale};
  Signature signature = {
      inout(arg_weights, "w"),
      input(arg_in, "in"),
      input(arg_deltas, "deltas"),
      input(arg_scale, "scale"),
  };

  TF_RETURN_IF_ERROR(res.graph_cache.ExecuteCached(inst, graph, res, seq, func,
                                                   signature, args));

  arg_weights = args[0];

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, arg_weights));

  return seq;
}

StatusOr<poplar::program::Program> CreateConvBiasAddOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence prog;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, prog));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor in = inputs[0][0];

  TF_ASSIGN_OR_RETURN(
      poplar::Tensor bias,
      FindInstructionInput(tensor_map, res, inst, 1, prog, false));

  const auto* conv_op = GetOperandLookThroughInterIpuCopy(inst, 0);
  poplar::Tensor shuffled_in = ShuffleConvolutionOutputToPoplar(conv_op, in);

  poplin::addBias(graph, shuffled_in, bias, prog, GetDebugName(inst));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, in));
  return prog;
}

StatusOr<poplar::program::Program> CreateBiasApply(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  const HloInstruction* root =
      inst->fused_instructions_computation()->root_instruction();

  // Find the biases.
  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor biases = inputs[0][0];

  // Find the deltas.
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor deltas,
      FindInstructionInput(tensor_map, res, inst, 1, seq, false));
  // Find the scale.
  TF_ASSIGN_OR_RETURN(poplar::Tensor scale,
                      FindInstructionInput(tensor_map, res, inst, 2, seq));

  // Find reduction dimensions
  const auto* reduce = root->operand(1)->operand(0);
  std::vector<std::size_t> reduction_dims;
  for (auto d : reduce->dimensions()) {
    reduction_dims.push_back(d);
  }

  using namespace poputil::graphfn;
  const std::string debug_prefix = GetDebugName(inst);
  auto func = [&graph, reduction_dims, debug_prefix](
                  std::vector<poplar::Tensor>& args,
                  poplar::program::Sequence& prog) {
    poplar::Tensor scale_float = args[2];
    if (scale_float.elementType() != poplar::FLOAT) {
      scale_float = popops::cast(graph, scale_float, poplar::FLOAT, prog,
                                 debug_prefix + "/ScaleToFloat");
    }
    // Reduce with scale and update in place
    popops::mapInPlace(graph, popops::expr::UnaryOpType::NEGATE, scale_float,
                       prog, debug_prefix + "/negate");
    popops::reduceWithOutput(graph, args[1], args[0], reduction_dims,
                             {popops::Operation::ADD, true, scale_float}, prog,
                             debug_prefix);
  };

  // Depending on whether this is performed inplace or not, the output could be
  // a new tensor or the biases tensor.
  std::vector<poplar::Tensor> args = {biases, deltas, scale};
  Signature signature = {inout(biases, "biases"), input(deltas, "deltas"),
                         input(scale, "scale")};

  TF_RETURN_IF_ERROR(res.graph_cache.ExecuteCached(inst, graph, res, seq, func,
                                                   signature, args));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, args[0]));

  return seq;
}

}  // namespace poplarplugin
}  // namespace xla
