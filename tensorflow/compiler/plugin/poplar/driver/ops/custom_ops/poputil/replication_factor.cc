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

#include <popops/ElementWise.hpp>
#include <poputil/TileMapping.hpp>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/custom_ops/poplar_ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

namespace pe = popops::expr;

namespace xla {
namespace poplarplugin {
namespace {
class ReplicationFactorOp : public PoplarOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    auto output = graph.addConstant(poplar::INT, {}, res.replication_factor,
                                    GetDebugName(inst));
    graph.setTileMapping(output, 0);

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, output));

    return poplar::program::Sequence();
  }
};
REGISTER_POPLAR_OP(ReplicationFactor, ReplicationFactorOp);

class ReplicationNormaliseOp : public PoplarOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    poplar::program::Sequence seq;

    // Get the inplace input.
    TF_ASSIGN_OR_RETURN(
        TensorVectors inputs,
        FindInplaceOutputTensors(tensor_map, res, inst, seq, false));
    CHECK_EQ(inputs.size(), 1);
    CHECK_EQ(inputs[0].size(), 1);
    poplar::Tensor inout = inputs[0][0];

    if (res.replication_factor > 1) {
      popops::mapInPlace(
          graph, pe::Divide(pe::_1, pe::Const(res.replication_factor)), {inout},
          seq, GetDebugName(inst) + "/replication_normalise");
    }

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, inout));

    return seq;
  }
};
REGISTER_POPLAR_OP(ReplicationNormalise, ReplicationNormaliseOp);

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
