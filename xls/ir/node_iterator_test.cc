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

#include "xls/ir/node_iterator.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/bits.h"
#include "xls/ir/function.h"
#include "xls/ir/function_base.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/package.h"

namespace xls {
namespace {

TEST(NodeIteratorTest, ReordersViaDependencies) {
  Package p("p");
  Function f("f", &p);
  SourceInfo loc;
  XLS_ASSERT_OK_AND_ASSIGN(Node * literal,
                           f.MakeNode<Literal>(loc, Value(UBits(3, 2))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * neg,
                           f.MakeNode<UnOp>(loc, literal, Op::kNeg));

  XLS_ASSERT_OK(f.set_return_value(neg));

  // Literal should precede the negation in RPO although we added those nodes in
  // the opposite order.
  NodeIterator rni = TopoSort(&f);
  auto it = rni.begin();
  EXPECT_EQ(*it, literal);
  ++it;
  EXPECT_EQ(*it, neg);
  ++it;
  EXPECT_EQ(rni.end(), it);
}

TEST(NodeIteratorTest, Diamond) {
  std::string program = R"(
  fn diamond(x: bits[32]) -> bits[32] {
    neg.1: bits[32] = neg(x)
    neg.2: bits[32] = neg(x)
    ret add.3: bits[32] = add(neg.1, neg.2)
  })";

  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  NodeIterator rni = TopoSort(f);
  auto it = rni.begin();
  EXPECT_EQ((*it)->GetName(), "x");
  ++it;
  EXPECT_EQ((*it)->GetName(), "neg.1");
  ++it;
  EXPECT_EQ((*it)->GetName(), "neg.2");
  ++it;
  EXPECT_EQ((*it)->GetName(), "add.3");
  ++it;
  EXPECT_EQ(rni.end(), it);
}

// Constructs a test as follows:
//
//        A
//      /   \
//      \    B
//       \  /
//        \/
//         C
//
// Topological order: A B C
TEST(NodeIteratorTest, PostOrderNotPreOrder) {
  Package p("p");
  Function f("f", &p);
  SourceInfo loc;
  XLS_ASSERT_OK_AND_ASSIGN(Node * a,
                           f.MakeNode<Literal>(loc, Value(UBits(0, 2))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b, f.MakeNode<BinOp>(loc, a, a, Op::kAdd));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c, f.MakeNode<BinOp>(loc, a, b, Op::kAdd));

  XLS_ASSERT_OK(f.set_return_value(c));

  NodeIterator rni = TopoSort(&f);
  auto it = rni.begin();
  EXPECT_EQ(*it, a);
  ++it;
  EXPECT_EQ(*it, b);
  ++it;
  EXPECT_EQ(*it, c);
  ++it;
  EXPECT_EQ(rni.end(), it);
}

// Constructs a test as follows:
//
//         A --
//        / \  \
//        | |   \
//        \ /   |
//         B    C
//          \  /
//            D
//
// Topo: D B C A =(reverse)=> A C B D
//                              2 1 3
TEST(NodeIteratorTest, TwoOfSameOperandLinks) {
  std::string program = R"(
  fn computation(a: bits[32]) -> bits[32] {
    b: bits[32] = add(a, a)
    c: bits[32] = neg(a)
    ret d: bits[32] = add(b, c)
  })";

  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  NodeIterator rni = TopoSort(f);
  auto it = rni.begin();
  EXPECT_EQ((*it)->GetName(), "a");
  ++it;
  EXPECT_EQ((*it)->GetName(), "b");
  ++it;
  EXPECT_EQ((*it)->GetName(), "c");
  ++it;
  EXPECT_EQ((*it)->GetName(), "d");
  ++it;
  EXPECT_EQ(rni.end(), it);
}

TEST(NodeIteratorTest, UselessParamsUnrelatedReturn) {
  std::string program = R"(
  fn computation(a: bits[32], b: bits[32]) -> bits[32] {
    ret r: bits[32] = literal(value=2)
  })";

  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  NodeIterator rni = TopoSort(f);
  auto it = rni.begin();
  EXPECT_EQ((*it)->GetName(), "a");
  ++it;
  EXPECT_EQ((*it)->GetName(), "b");
  ++it;
  EXPECT_EQ((*it)->GetName(), "r");
  ++it;
  EXPECT_EQ(rni.end(), it);
}

// Constructs a test as follows:
//
//      A
//     / \
//    T   C
//     \ / \
//      B   E
//       \ /
//        D
TEST(NodeIteratorTest, ExtendedDiamond) {
  std::string program = R"(
  fn computation(a: bits[32]) -> bits[32] {
    t: bits[32] = neg(a)
    c: bits[32] = neg(a)
    b: bits[32] = add(t, c)
    e: bits[32] = neg(c)
    ret d: bits[32] = add(b, e)
  })";

  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  NodeIterator rni = TopoSort(f);
  auto it = rni.begin();
  EXPECT_EQ((*it)->GetName(), "a");
  ++it;
  EXPECT_EQ((*it)->GetName(), "t");
  ++it;
  EXPECT_EQ((*it)->GetName(), "c");
  ++it;
  EXPECT_EQ((*it)->GetName(), "b");
  ++it;
  EXPECT_EQ((*it)->GetName(), "e");
  ++it;
  EXPECT_EQ((*it)->GetName(), "d");
  ++it;
  EXPECT_EQ(rni.end(), it);
}

TEST(NodeIteratorTest, ExtendedDiamondReverse) {
  std::string program = R"(
  fn computation(a: bits[32]) -> bits[32] {
    t: bits[32] = neg(a)
    c: bits[32] = neg(a)
    b: bits[32] = add(t, c)
    e: bits[32] = neg(c)
    ret d: bits[32] = add(b, e)
  })";
  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  // ReverseTopoSort should produce the same order but in reverse.
  NodeIterator fwd_it = TopoSort(f);
  NodeIterator rev_it = ReverseTopoSort(f);
  std::vector<Node*> fwd(fwd_it.begin(), fwd_it.end());
  std::vector<Node*> rev(rev_it.begin(), rev_it.end());
  std::reverse(fwd.begin(), fwd.end());
  EXPECT_EQ(fwd, rev);
}

// Constructs a test as follows:
//
//      D
//      | \
//      C  \
//      |   \
//      B    T
//       \  /
//        \/
//         A
//
// Given that we know we visit operands in left-to-right order, this example
// points out the discrepancy between the RPO ordering and what our algorithm
// produces. The depth-first traversal RPO necessitates would have us visit the
// whole D,C,B chain before T.
//
// Post-Order:     D C B T A =(rev)=> A T B C D
//                                      1 2 3 4
// Our topo order: D T C B A =(rev)=> A B C T D
//                                      2 3 1 4
TEST(NodeIteratorTest, RpoVsTopo) {
  std::string program = R"(
  fn computation(a: bits[32]) -> bits[32] {
    t: bits[32] = neg(a)
    b: bits[32] = neg(a)
    c: bits[32] = neg(b)
    ret d: bits[32] = add(c, t)
  })";

  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, Parser::ParseFunction(program, &p));

  NodeIterator rni = TopoSort(f);
  auto it = rni.begin();
  EXPECT_EQ((*it)->GetName(), "a");
  ++it;
  EXPECT_EQ((*it)->GetName(), "b");
  ++it;
  EXPECT_EQ((*it)->GetName(), "c");
  ++it;
  EXPECT_EQ((*it)->GetName(), "t");
  ++it;
  EXPECT_EQ((*it)->GetName(), "d");
  ++it;
  EXPECT_EQ(rni.end(), it);
}

template <typename MakeLeaf>
BValue MakeBalancedBinaryTree(FunctionBuilder& fb, int64_t depth,
                              MakeLeaf make_leaf) {
  if (depth == 0) {
    return make_leaf();
  }
  BValue l = MakeBalancedBinaryTree(fb, depth - 1, make_leaf);
  BValue r = MakeBalancedBinaryTree(fb, depth - 1, make_leaf);
  return fb.Add(l, r);
}

void BM_TopoSortBinaryTree(benchmark::State& state) {
  std::unique_ptr<VerifiedPackage> p =
      std::make_unique<VerifiedPackage>("balanced_tree_pkg");
  FunctionBuilder fb("balanced_tree", p.get());
  MakeBalancedBinaryTree(fb, state.range(0),
                         [&]() { return fb.Literal(UBits(42, 8)); });
  auto fs = fb.Build();
  XLS_CHECK_OK(fs.status());
  FunctionBase* f = fs.value();
  for (auto _ : state) {
    auto v = TopoSort(f);
    benchmark::DoNotOptimize(v);
  }
}

// Wide and dense
// Fully connected layers of a given width.
void BM_TopoSortDense(benchmark::State& state) {
  std::unique_ptr<VerifiedPackage> p =
      std::make_unique<VerifiedPackage>("dense_tree_pkg");
  FunctionBuilder fb("dense_tree", p.get());
  int64_t depth = state.range(0);
  int64_t width = state.range(1);
  std::vector<BValue> prev_layer;
  prev_layer.reserve(width);
  for (int64_t i = 0; i < width; ++i) {
    prev_layer.push_back(fb.Literal(UBits(1, 8)));
  }
  for (int64_t i = 0; i < depth; ++i) {
    std::vector<BValue> next_layer;
    next_layer.reserve(width);
    for (int64_t j = 0; j < width; ++j) {
      next_layer.push_back(fb.Select(
          prev_layer[0], absl::MakeSpan(prev_layer).subspan(2), prev_layer[1]));
    }
    prev_layer = std::move(next_layer);
  }
  fb.Select(prev_layer[0], absl::MakeSpan(prev_layer).subspan(2),
            prev_layer[1]);
  auto fs = fb.Build();
  XLS_CHECK_OK(fs.status());
  FunctionBase* f = fs.value();
  for (auto _ : state) {
    auto v = TopoSort(f);
    benchmark::DoNotOptimize(v);
  }
}

// Just a very deep ladder structure
// ...
// x_{n-2} : x_{n-3} + 1
// x_n := x_{n-1} + 1
// ...
void BM_TopoSortLadder(benchmark::State& state) {
  std::unique_ptr<VerifiedPackage> p =
      std::make_unique<VerifiedPackage>("ladder_tree_pkg");
  FunctionBuilder fb("ladder_tree", p.get());
  BValue last = fb.Literal(UBits(1, 8));
  for (int64_t i = 0; i < state.range(0); ++i) {
    last = fb.Add(last, fb.Literal(UBits(1, 8)));
  }
  auto fs = fb.Build();
  XLS_CHECK_OK(fs.status());
  FunctionBase* f = fs.value();
  for (auto _ : state) {
    auto v = TopoSort(f);
    benchmark::DoNotOptimize(v);
  }
}

BENCHMARK(BM_TopoSortBinaryTree)->DenseRange(2, 20, 2);
BENCHMARK(BM_TopoSortLadder)->Range(2, 1024);
BENCHMARK(BM_TopoSortDense)->RangePair(2, 512, 3, 32);

}  // namespace
}  // namespace xls
