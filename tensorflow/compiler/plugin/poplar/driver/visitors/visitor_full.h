/* Copyright 2017 Graphcore Ltd
 */

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_FULL_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_FULL_H_

#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_base.h"

namespace xla {
namespace poplarplugin {

/*
 * The full visitor is an extension of the base visitor
 * that adds other operations which do element to element
 * mixing, for instance convolution.  It also adds ops
 * that change the shape of the tensor, for instance Reverse
 * or Concatinate.
 */
class FullVisitor : public BaseVisitor {
 public:
  FullVisitor(CompilerResources&);

  Status HandleConcatenate(HloInstruction* inst) override;

  Status HandleDot(HloInstruction* inst) override;

  Status HandleConvolution(HloInstruction* inst) override;

  Status HandleReverse(HloInstruction* inst) override;

  Status HandleReduce(HloInstruction* inst) override;

  Status HandleBitcast(HloInstruction* inst) override;

  Status HandleBroadcast(HloInstruction* inst) override;

  Status HandleReshape(HloInstruction* inst) override;

  Status HandleTranspose(HloInstruction* inst) override;

  Status HandleSlice(HloInstruction* inst) override;

  Status HandleDynamicSlice(HloInstruction* inst) override;

  Status HandleDynamicUpdateSlice(HloInstruction* inst) override;

  Status HandleReduceWindow(HloInstruction* inst) override;

  Status HandleScatter(HloInstruction* inst) override;

  Status HandleSelectAndScatter(HloInstruction* inst) override;

  Status HandleWhile(HloInstruction* inst) override;

  Status HandlePad(HloInstruction* inst) override;

  Status HandleIota(HloInstruction* inst) override;

  Status HandleSort(HloInstruction* inst) override;

  Status HandleBatchNormInference(HloInstruction* inst) override;

  Status HandleBatchNormTraining(HloInstruction* inst) override;

  Status HandleBatchNormGrad(HloInstruction* inst) override;

  Status Postprocess(HloInstruction* inst) override;

  Status HandleGather(HloInstruction* inst) override;
};

}  // namespace poplarplugin
}  // namespace xla

#endif