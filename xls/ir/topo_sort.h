// Copyright 2020 The XLS Authors
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

#ifndef XLS_IR_NODE_ITERATOR_H_
#define XLS_IR_NODE_ITERATOR_H_

#include <vector>

#include "xls/ir/function_base.h"
#include "xls/ir/node.h"

namespace xls {

// Convenience function for concise use in foreach constructs; e.g.:
//
//  for (Node* n : TopoSort(f)) {
//    ...
//  }
//
// Yields nodes in a stable topological traversal order (dependency ordering is
// satisfied).
//
// Note that the ordering for all nodes is computed up front, *not*
// incrementally as iteration proceeds.
std::vector<Node*> TopoSort(FunctionBase* f);

// As above, but returns a reverse topo order.
std::vector<Node*> ReverseTopoSort(FunctionBase* f);

}  // namespace xls

#endif  // XLS_IR_NODE_ITERATOR_H_
