//===- CgSpillPlacement.cpp - Optimal Spill Code Placement
//------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the spill code placement analysis.
//
// Each edge bundle corresponds to a node in a Hopfield network. Constraints on
// basic blocks are weighted by the block frequency and added to become the node
// bias.
//
// Transparent basic blocks have the variable live through, but don't care if it
// is spilled or in a register. These blocks become connections in the Hopfield
// network, again weighted by block frequency.
//
// The Hopfield network minimizes (possibly locally) its energy function:
//
//   E = -sum_n V_n * ( B_n + sum_{n, m linked by b} V_m * F_b )
//
// The energy function represents the expected spill code execution frequency,
// or the cost of spilling. This is a Lyapunov function which never increases
// when a node is updated. It is guaranteed to converge to a local minimum.
//
//===----------------------------------------------------------------------===//

#include "spill_placement.h"

#include "llvm/ADT/BitVector.h"

#include "cg_block_frequency_info.h"
#include "cg_loop_info.h"
#include "cgir/cg_basic_block.h"
#include "cgir/cg_function.h"
#include "edge_bundles.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>

using namespace llvm;
using namespace COMPILER;

#define DEBUG_TYPE "spill-code-placement"

/// Node - Each edge bundle corresponds to a Hopfield node.
///
/// The node contains precomputed frequency data that only depends on the CFG,
/// but Bias and Links are computed each time placeSpills is called.
///
/// The node Value is positive when the variable should be in a register. The
/// value can change when linked nodes change, but convergence is very fast
/// because all weights are positive.
struct CgSpillPlacement::Node {
  /// BiasN - Sum of blocks that prefer a spill.
  BlockFrequency BiasN;

  /// BiasP - Sum of blocks that prefer a register.
  BlockFrequency BiasP;

  /// Value - Output value of this node computed from the Bias and links.
  /// This is always on of the values {-1, 0, 1}. A positive number means the
  /// variable should go in a register through this bundle.
  int Value;

  using LinkVector = SmallVector<std::pair<BlockFrequency, unsigned>, 4>;

  /// Links - (Weight, BundleNo) for all transparent blocks connecting to other
  /// bundles. The weights are all positive block frequencies.
  LinkVector Links;

  /// SumLinkWeights - Cached sum of the weights of all links + ThresHold.
  BlockFrequency SumLinkWeights;

  /// preferReg - Return true when this node prefers to be in a register.
  bool preferReg() const {
    // Undecided nodes (Value==0) go on the stack.
    return Value > 0;
  }

  /// mustSpill - Return True if this node is so biased that it must spill.
  bool mustSpill() const {
    // We must spill if Bias < -sum(weights) or the MustSpill flag was set.
    // BiasN is saturated when MustSpill is set, make sure this still returns
    // true when the RHS saturates. Note that SumLinkWeights includes Threshold.
    return BiasN >= BiasP + SumLinkWeights;
  }

  /// clear - Reset per-query data, but preserve frequencies that only depend on
  /// the CFG.
  void clear(const BlockFrequency &Threshold) {
    BiasN = BiasP = Value = 0;
    SumLinkWeights = Threshold;
    Links.clear();
  }

  /// addLink - Add a link to bundle b with weight w.
  void addLink(unsigned b, BlockFrequency w) {
    // Update cached sum.
    SumLinkWeights += w;

    // There can be multiple links to the same bundle, add them up.
    for (std::pair<BlockFrequency, unsigned> &L : Links)
      if (L.second == b) {
        L.first += w;
        return;
      }
    // This must be the first link to b.
    Links.push_back(std::make_pair(w, b));
  }

  /// addBias - Bias this node.
  void addBias(BlockFrequency freq, BorderConstraint direction) {
    switch (direction) {
    default:
      break;
    case PrefReg:
      BiasP += freq;
      break;
    case PrefSpill:
      BiasN += freq;
      break;
    case MustSpill:
      BiasN = BlockFrequency::getMaxFrequency();
      break;
    }
  }

  /// update - Recompute Value from Bias and Links. Return true when node
  /// preference changes.
  bool update(const Node nodes[], const BlockFrequency &Threshold) {
    // Compute the weighted sum of inputs.
    BlockFrequency SumN = BiasN;
    BlockFrequency SumP = BiasP;
    for (std::pair<BlockFrequency, unsigned> &L : Links) {
      if (nodes[L.second].Value == -1)
        SumN += L.first;
      else if (nodes[L.second].Value == 1)
        SumP += L.first;
    }

    // Each weighted sum is going to be less than the total frequency of the
    // bundle. Ideally, we should simply set Value = sign(SumP - SumN), but we
    // will add a dead zone around 0 for two reasons:
    //
    //  1. It avoids arbitrary bias when all links are 0 as is possible during
    //     initial iterations.
    //  2. It helps tame rounding errors when the links nominally sum to 0.
    //
    bool Before = preferReg();
    if (SumN >= SumP + Threshold)
      Value = -1;
    else if (SumP >= SumN + Threshold)
      Value = 1;
    else
      Value = 0;
    return Before != preferReg();
  }

  void getDissentingNeighbors(SparseSet<unsigned> &List,
                              const Node nodes[]) const {
    for (const auto &Elt : Links) {
      unsigned n = Elt.second;
      // Neighbors that already have the same value are not going to
      // change because of this node changing.
      if (Value != nodes[n].Value)
        List.insert(n);
    }
  }
};

bool CgSpillPlacement::runOnCgFunction(CgFunction &mf) {
  MF = &mf;
  bundles = mf.EdgeBundles;
  loops = mf.Loops;

  assert(!nodes && "Leaking node array");
  nodes = new Node[bundles->getNumBundles()];
  TodoList.clear();
  TodoList.setUniverse(bundles->getNumBundles());

  // Compute total ingoing and outgoing block frequencies for all bundles.
  BlockFrequencies.resize(mf.getNumBlockIDs());
  MBFI = mf.MBFI;
  setThreshold(MBFI->getEntryFreq());
  for (auto *I : mf) {
    unsigned Num = I->getNumber();
    BlockFrequencies[Num] = MBFI->getBlockFreq(I);
  }

  mf.SpillPlacer = this;

  // We never change the function.
  return false;
}

void CgSpillPlacement::releaseMemory() {
  delete[] nodes;
  nodes = nullptr;
  TodoList.clear();
}

/// activate - mark node n as active if it wasn't already.
void CgSpillPlacement::activate(unsigned n) {
  TodoList.insert(n);
  if (ActiveNodes->test(n))
    return;
  ActiveNodes->set(n);
  nodes[n].clear(Threshold);

  // Very large bundles usually come from big switches, indirect branches,
  // landing pads, or loops with many 'continue' statements. It is difficult to
  // allocate registers when so many different blocks are involved.
  //
  // Give a small negative bias to large bundles such that a substantial
  // fraction of the connected blocks need to be interested before we consider
  // expanding the region through the bundle. This helps compile time by
  // limiting the number of blocks visited and the number of links in the
  // Hopfield network.
  if (bundles->getBlocks(n).size() > 100) {
    nodes[n].BiasP = 0;
    nodes[n].BiasN = (MBFI->getEntryFreq() / 16);
  }
}

/// Set the threshold for a given entry frequency.
///
/// Set the threshold relative to \c Entry.  Since the threshold is used as a
/// bound on the open interval (-Threshold;Threshold), 1 is the minimum
/// threshold.
void CgSpillPlacement::setThreshold(const BlockFrequency &Entry) {
  // Apparently 2 is a good threshold when Entry==2^14, but we need to scale
  // it.  Divide by 2^13, rounding as appropriate.
  uint64_t Freq = Entry.getFrequency();
  uint64_t Scaled = (Freq >> 13) + bool(Freq & (1 << 12));
  Threshold = std::max(UINT64_C(1), Scaled);
}

/// addConstraints - Compute node biases and weights from a set of constraints.
/// Set a bit in NodeMask for each active node.
void CgSpillPlacement::addConstraints(ArrayRef<BlockConstraint> LiveBlocks) {
  for (const BlockConstraint &LB : LiveBlocks) {
    BlockFrequency Freq = BlockFrequencies[LB.Number];

    // Live-in to block?
    if (LB.Entry != DontCare) {
      unsigned ib = bundles->getBundle(LB.Number, false);
      activate(ib);
      nodes[ib].addBias(Freq, LB.Entry);
    }

    // Live-out from block?
    if (LB.Exit != DontCare) {
      unsigned ob = bundles->getBundle(LB.Number, true);
      activate(ob);
      nodes[ob].addBias(Freq, LB.Exit);
    }
  }
}

/// addPrefSpill - Same as addConstraints(PrefSpill)
void CgSpillPlacement::addPrefSpill(ArrayRef<unsigned> Blocks, bool Strong) {
  for (unsigned B : Blocks) {
    BlockFrequency Freq = BlockFrequencies[B];
    if (Strong)
      Freq += Freq;
    unsigned ib = bundles->getBundle(B, false);
    unsigned ob = bundles->getBundle(B, true);
    activate(ib);
    activate(ob);
    nodes[ib].addBias(Freq, PrefSpill);
    nodes[ob].addBias(Freq, PrefSpill);
  }
}

void CgSpillPlacement::addLinks(ArrayRef<unsigned> Links) {
  for (unsigned Number : Links) {
    unsigned ib = bundles->getBundle(Number, false);
    unsigned ob = bundles->getBundle(Number, true);

    // Ignore self-loops.
    if (ib == ob)
      continue;
    activate(ib);
    activate(ob);
    BlockFrequency Freq = BlockFrequencies[Number];
    nodes[ib].addLink(ob, Freq);
    nodes[ob].addLink(ib, Freq);
  }
}

bool CgSpillPlacement::scanActiveBundles() {
  RecentPositive.clear();
  for (unsigned n : ActiveNodes->set_bits()) {
    update(n);
    // A node that must spill, or a node without any links is not going to
    // change its value ever again, so exclude it from iterations.
    if (nodes[n].mustSpill())
      continue;
    if (nodes[n].preferReg())
      RecentPositive.push_back(n);
  }
  return !RecentPositive.empty();
}

bool CgSpillPlacement::update(unsigned n) {
  if (!nodes[n].update(nodes, Threshold))
    return false;
  nodes[n].getDissentingNeighbors(TodoList, nodes);
  return true;
}

/// iterate - Repeatedly update the Hopfield nodes until stability or the
/// maximum number of iterations is reached.
void CgSpillPlacement::iterate() {
  // We do not need to push those node in the todolist.
  // They are already been proceeded as part of the previous iteration.
  RecentPositive.clear();

  // Since the last iteration, the todolist have been augmented by calls
  // to addConstraints, addLinks, and co.
  // Update the network energy starting at this new frontier.
  // The call to ::update will add the nodes that changed into the todolist.
  unsigned Limit = bundles->getNumBundles() * 10;
  while (Limit-- > 0 && !TodoList.empty()) {
    unsigned n = TodoList.pop_back_val();
    if (!update(n))
      continue;
    if (nodes[n].preferReg())
      RecentPositive.push_back(n);
  }
}

void CgSpillPlacement::prepare(BitVector &RegBundles) {
  RecentPositive.clear();
  TodoList.clear();
  // Reuse RegBundles as our ActiveNodes vector.
  ActiveNodes = &RegBundles;
  ActiveNodes->clear();
  ActiveNodes->resize(bundles->getNumBundles());
}

bool CgSpillPlacement::finish() {
  assert(ActiveNodes && "Call prepare() first");

  // Write preferences back to ActiveNodes.
  bool Perfect = true;
  for (unsigned n : ActiveNodes->set_bits())
    if (!nodes[n].preferReg()) {
      ActiveNodes->reset(n);
      Perfect = false;
    }
  ActiveNodes = nullptr;
  return Perfect;
}

void CgSpillPlacement::BlockConstraint::print(raw_ostream &OS) const {
  auto toString = [](BorderConstraint C) -> StringRef {
    switch (C) {
    case DontCare:
      return "DontCare";
    case PrefReg:
      return "PrefReg";
    case PrefSpill:
      return "PrefSpill";
    case PrefBoth:
      return "PrefBoth";
    case MustSpill:
      return "MustSpill";
    };
    llvm_unreachable("uncovered switch");
  };

  dbgs() << "{" << Number << ", " << toString(Entry) << ", " << toString(Exit)
         << ", " << (ChangesValue ? "changes" : "no change") << "}";
}

void CgSpillPlacement::BlockConstraint::dump() const {
  print(dbgs());
  dbgs() << "\n";
}
