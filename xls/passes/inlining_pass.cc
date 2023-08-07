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

#include "xls/passes/inlining_pass.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/call_graph.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/nodes.h"

namespace xls {
namespace {

// Return the name that 'node' should have when it is inlined at the callsite
// given by 'invoke'. The node must be in the function called by 'invoke'. The
// name is generated by first determining if the name of 'node' is likely
// derived from the parameter name of its function. If so, a new name is
// generated using the respective operand name of 'invoke' substituted for the
// parameter name. If no meaningful name could be determined then nullopt is
// returned.
std::optional<std::string> GetInlinedNodeName(Node* node, Invoke* invoke) {
  if (!node->HasAssignedName()) {
    return std::nullopt;
  }

  // Find the parameter in the invoked function whose name is a prefix of the
  // name of 'node'. If such a parameter exists we assume the name of 'node' is
  // derived from the name of this param. If there are multiple matches, then
  // choose the param with the longest name (e.g., if the node's name is
  // 'foo_bar_42' and there are parameters named 'foo' and 'foo_bar' we assume
  // 'foo_bar_42' is derived from 'foo_bar').
  Param* matching_param = nullptr;
  std::string derived_name;
  for (int64_t i = 0; i < invoke->operand_count(); ++i) {
    Param* param = invoke->to_apply()->param(i);
    Node* operand = invoke->operand(i);
    if (operand->HasAssignedName() &&
        absl::StartsWith(node->GetName(), param->GetName()) &&
        (matching_param == nullptr ||
         matching_param->GetName().size() < param->GetName().size())) {
      matching_param = param;
      std::string suffix = node->GetName().substr(param->GetName().size());
      derived_name = absl::StrCat(operand->GetName(), suffix);
    }
  }
  if (matching_param == nullptr) {
    return std::nullopt;
  }
  return derived_name;
}

// Inlining can cause coverpoints to be duplicated, which will then conflict, as
// a Verilog cover property must have a unique name. To handle this, we prepend
// the original node ID to an inlined coverpoint. Post-processing will be needed
// to re-aggregate coverpoints disaggregated in this method.
std::string GetPrefixedLabel(Invoke* invoke, std::string_view label,
                             int inline_count) {
  FunctionBase* caller = invoke->function_base();
  return absl::StrCat(caller->name(), "_", inline_count, "_",
                      invoke->to_apply()->name(), "_", label);
}

static bool IsInlineable(const Invoke* invoke) {
  // Foreign functions can not and should not be inlined.
  return !invoke->to_apply()->ForeignFunctionData().has_value();
}

// Inlines the node "invoke" by replacing it with the contents of the called
// function.
absl::Status InlineInvoke(Invoke* invoke, int inline_count) {
  Function* invoked = invoke->to_apply();
  absl::flat_hash_map<Node*, Node*> invoked_node_to_replacement;
  for (int64_t i = 0; i < invoked->params().size(); ++i) {
    Node* param = invoked->param(i);
    invoked_node_to_replacement[param] = invoke->operand(i);
  }

  for (Node* node : TopoSort(invoked)) {
    if (invoked_node_to_replacement.contains(node)) {
      // Already taken care of (e.g. parameters above).
      continue;
    }
    XLS_RET_CHECK(  // All invokes before us should've been inlined (except ffi)
        !node->Is<Invoke>() || !IsInlineable(node->As<Invoke>()))
        << "No invokes that are not FFI should remain in function to inline: "
        << node->GetName() << ": " << node->As<Invoke>()->to_apply()->name();
    std::vector<Node*> new_operands;
    for (Node* operand : node->operands()) {
      new_operands.push_back(invoked_node_to_replacement.at(operand));
    }
    XLS_ASSIGN_OR_RETURN(
        Node * new_node,
        node->CloneInNewFunction(new_operands, invoke->function_base()));
    if (new_node->loc().Empty()) {
      new_node->SetLoc(invoke->loc());
    }
    invoked_node_to_replacement[node] = new_node;
  }

  // Update names for each of the newly inlined nodes. For example,
  // if the callsite looks like:
  //
  //   invoke.1: invoke(foo, to_apply=f)
  //
  // and the function is:
  //
  //   fn f(x: bits[32]) -> bits[32] {
  //     ...
  //     x_negated: bits[32] = neg(x)
  //     ...
  //   }
  //
  // Then 'x_negated' when inlined at the invoke callsite will have the name
  // 'foo_negated'. Coverpoint and assert names are also updated to include the
  // call stack to differentiate in case inlining would otherwise result in
  // multiple statements with the same labels.
  for (Node* node : invoked->nodes()) {
    if (node->Is<Param>()) {
      continue;
    }
    if (node == invoked->return_value() && invoke->HasAssignedName()) {
      // Node is the return value of the function, it should get its name from
      // the invoke node itself. By clearing the name here ReplaceUseWith will
      // properly move the name from the invoke instruction to the node.
      invoked_node_to_replacement.at(node)->ClearName();
      continue;
    }
    std::optional<std::string> new_name = GetInlinedNodeName(node, invoke);
    if (new_name.has_value()) {
      invoked_node_to_replacement.at(node)->SetName(new_name.value());
    }

    if (node->Is<Cover>()) {
      std::string new_label =
          GetPrefixedLabel(invoke, node->As<Cover>()->label(), inline_count);
      Cover* cover = invoked_node_to_replacement.at(node)->As<Cover>();
      Node* token = cover->token();
      Node* condition = cover->condition();
      XLS_ASSIGN_OR_RETURN(
          auto new_cover,
          cover->function_base()->MakeNodeWithName<Cover>(
              cover->loc(), token, condition, new_label, cover->GetName()));
      XLS_RETURN_IF_ERROR(cover->ReplaceUsesWith(new_cover));
      XLS_RETURN_IF_ERROR(cover->function_base()->RemoveNode(cover));
      invoked_node_to_replacement.at(node) = new_cover;
    } else if (node->Is<Assert>() && node->As<Assert>()->label().has_value()) {
      std::string new_label = GetPrefixedLabel(
          invoke, node->As<Assert>()->label().value(), inline_count);
      Assert* asrt = invoked_node_to_replacement.at(node)->As<Assert>();
      Node* token = asrt->token();
      Node* condition = asrt->condition();
      XLS_ASSIGN_OR_RETURN(auto new_assert,
                           asrt->function_base()->MakeNodeWithName<Assert>(
                               asrt->loc(), token, condition, asrt->message(),
                               new_label, asrt->GetName()));
      XLS_RETURN_IF_ERROR(asrt->ReplaceUsesWith(new_assert));
      XLS_RETURN_IF_ERROR(asrt->function_base()->RemoveNode(asrt));
      invoked_node_to_replacement.at(node) = new_assert;
    }
  }

  XLS_RETURN_IF_ERROR(invoke->ReplaceUsesWith(
      invoked_node_to_replacement.at(invoked->return_value())));
  return invoke->function_base()->RemoveNode(invoke);
}

}  // namespace

absl::StatusOr<bool> InliningPass::RunInternal(Package* p,
                                               const PassOptions& options,
                                               PassResults* results) const {
  bool changed = false;
  // Inline all the invokes of each function where functions are processed in a
  // post order of the call graph (leaves first). This ensures that when a
  // function Foo is inlined into its callsites, no invokes remain in Foo. This
  // avoid duplicate work.
  int inline_count = 0;
  for (FunctionBase* f : FunctionsInPostOrder(p)) {
    // Create copy of nodes() because we will be adding and removing nodes
    // during inlining.
    std::vector<Node*> nodes(f->nodes().begin(), f->nodes().end());
    for (Node* node : nodes) {
      if (node->Is<Invoke>() && IsInlineable(node->As<Invoke>())) {
        XLS_RETURN_IF_ERROR(InlineInvoke(node->As<Invoke>(), inline_count++));
        changed = true;
      }
    }
  }
  return changed;
}

}  // namespace xls
