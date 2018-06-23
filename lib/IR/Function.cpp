//===- Function.cpp - MLIR Function Classes -------------------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "mlir/IR/CFGFunction.h"
#include "llvm/ADT/StringRef.h"
using namespace mlir;

Function::Function(StringRef name, FunctionType *type, Kind kind)
  : kind(kind), name(name.str()), type(type) {
}

//===----------------------------------------------------------------------===//
// ExtFunction implementation.
//===----------------------------------------------------------------------===//

ExtFunction::ExtFunction(StringRef name, FunctionType *type)
  : Function(name, type, Kind::ExtFunc) {
}

//===----------------------------------------------------------------------===//
// CFGFunction implementation.
//===----------------------------------------------------------------------===//

CFGFunction::CFGFunction(StringRef name, FunctionType *type)
  : Function(name, type, Kind::CFGFunc) {
}
