//===- CgLiveIntervals.h - Live Interval Analysis -----------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file This file implements the CgLiveInterval analysis pass.  Given some
/// numbering of each the machine instructions (in this implemention depth-first
/// order) an interval [i, j) is said to be a live interval for register v if
/// there is no instruction with number j' > j such that v is live at j' and
/// there is no instruction with number i' < i such that v is live at i'. In
/// this implementation intervals can have holes, i.e. an interval might look
/// like [1,20), [50,65), [1000,1001).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "compiler/cgir/cg_basic_block.h"
#include "compiler/cgir/pass/live_interval.h"
#include "compiler/cgir/pass/slot_indexes.h"
#include "compiler/context.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/MC/LaneBitmask.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <cstdint>
#include <utility>

namespace llvm {
class BitVector;
class raw_ostream;
class TargetInstrInfo;
} // namespace llvm

namespace COMPILER {

constexpr bool UseSegmentSetForPhysRegs = true;

class CgLiveIntervalCalc;
class CgBlockFrequencyInfo;
class CgDominatorTree;
class CgFunction;
class CgInstruction;
class CgRegisterInfo;
class CgVirtRegMap;

class CgLiveIntervals : public ContextObject {
  CgFunction *MF;
  CgRegisterInfo *MRI;
  const TargetRegisterInfo *TRI;
  const TargetInstrInfo *TII;
  CgSlotIndexes *Indexes;
  CgDominatorTree *DomTree = nullptr;
  CgLiveIntervalCalc *LICalc = nullptr;

  /// Special pool allocator for CgVNInfo's (CgLiveInterval val#).
  CgVNInfo::Allocator VNInfoAllocator;

  /// Live interval pointers for all the virtual registers.
  IndexedMap<CgLiveInterval *, VirtReg2IndexFunctor> VirtRegIntervals;

  /// Sorted list of instructions with register mask operands. Always use the
  /// 'r' slot, RegMasks are normal clobbers, not early clobbers.
  SmallVector<CgSlotIndex, 8> RegMaskSlots;

  /// This vector is parallel to RegMaskSlots, it holds a pointer to the
  /// corresponding register mask.  This pointer can be recomputed as:
  ///
  ///   MI = Indexes->getInstructionFromIndex(RegMaskSlot[N]);
  ///   unsigned OpNum = findRegMaskOperand(MI);
  ///   RegMaskBits[N] = MI->getOperand(OpNum).getRegMask();
  ///
  /// This is kept in a separate vector partly because some standard
  /// libraries don't support lower_bound() with mixed objects, partly to
  /// improve locality when searching in RegMaskSlots.
  /// Also see the comment in CgLiveInterval::find().
  SmallVector<const uint32_t *, 8> RegMaskBits;

  /// For each basic block number, keep (begin, size) pairs indexing into the
  /// RegMaskSlots and RegMaskBits arrays.
  /// Note that basic block numbers may not be layout contiguous, that's why
  /// we can't just keep track of the first register mask in each basic
  /// block.
  SmallVector<std::pair<unsigned, unsigned>, 8> RegMaskBlocks;

  /// Keeps a live range set for each register unit to track fixed physreg
  /// interference.
  SmallVector<CgLiveRange *, 0> RegUnitRanges;

public:
  CgLiveIntervals(CgFunction &MF);
  ~CgLiveIntervals();

  /// Calculate the spill weight to assign to a single instruction.
  static float getSpillWeight(bool isDef, bool isUse,
                              const CgBlockFrequencyInfo *MBFI,
                              const CgInstruction &MI);

  /// Calculate the spill weight to assign to a single instruction.
  static float getSpillWeight(bool isDef, bool isUse,
                              const CgBlockFrequencyInfo *MBFI,
                              const CgBasicBlock *MBB);

  CgLiveInterval &getInterval(Register Reg) {
    if (hasInterval(Reg))
      return *VirtRegIntervals[Reg.id()];

    return createAndComputeVirtRegInterval(Reg);
  }

  const CgLiveInterval &getInterval(Register Reg) const {
    return const_cast<CgLiveIntervals *>(this)->getInterval(Reg);
  }

  bool hasInterval(Register Reg) const {
    return VirtRegIntervals.inBounds(Reg.id()) && VirtRegIntervals[Reg.id()];
  }

  /// Interval creation.
  CgLiveInterval &createEmptyInterval(Register Reg) {
    assert(!hasInterval(Reg) && "Interval already exists!");
    VirtRegIntervals.grow(Reg.id());
    VirtRegIntervals[Reg.id()] = createInterval(Reg);
    return *VirtRegIntervals[Reg.id()];
  }

  CgLiveInterval &createAndComputeVirtRegInterval(Register Reg) {
    CgLiveInterval &LI = createEmptyInterval(Reg);
    computeVirtRegInterval(LI);
    return LI;
  }

  /// Interval removal.
  void removeInterval(Register Reg) {
    deleteObject(VirtRegIntervals[Reg]);
    VirtRegIntervals[Reg] = nullptr;
  }

  /// Given a register and an instruction, adds a live segment from that
  /// instruction to the end of its MBB.
  CgLiveInterval::Segment addSegmentToEndOfBlock(Register Reg,
                                                 CgInstruction &startInst);

  /// After removing some uses of a register, shrink its live range to just
  /// the remaining uses. This method does not compute reaching defs for new
  /// uses, and it doesn't remove dead defs.
  /// Dead PHIDef values are marked as unused. New dead machine instructions
  /// are added to the dead vector. Returns true if the interval may have been
  /// separated into multiple connected components.
  bool shrinkToUses(CgLiveInterval *li,
                    SmallVectorImpl<CgInstruction *> *dead = nullptr);

  /// Specialized version of
  /// shrinkToUses(CgLiveInterval *li, SmallVectorImpl<CgInstruction*> *dead)
  /// that works on a subregister live range and only looks at uses matching
  /// the lane mask of the subregister range.
  /// This may leave the subrange empty which needs to be cleaned up with
  /// CgLiveInterval::removeEmptySubranges() afterwards.
  void shrinkToUses(CgLiveInterval::SubRange &SR, Register Reg);

  /// Extend the live range \p LR to reach all points in \p Indices. The
  /// points in the \p Indices array must be jointly dominated by the union
  /// of the existing defs in \p LR and points in \p Undefs.
  ///
  /// PHI-defs are added as needed to maintain SSA form.
  ///
  /// If a CgSlotIndex in \p Indices is the end index of a basic block, \p LR
  /// will be extended to be live out of the basic block.
  /// If a CgSlotIndex in \p Indices is jointy dominated only by points in
  /// \p Undefs, the live range will not be extended to that point.
  ///
  /// See also LiveRangeCalc::extend().
  void extendToIndices(CgLiveRange &LR, ArrayRef<CgSlotIndex> Indices,
                       ArrayRef<CgSlotIndex> Undefs);

  void extendToIndices(CgLiveRange &LR, ArrayRef<CgSlotIndex> Indices) {
    extendToIndices(LR, Indices, /*Undefs=*/{});
  }

  /// If \p LR has a live value at \p Kill, prune its live range by removing
  /// any liveness reachable from Kill. Add live range end points to
  /// EndPoints such that extendToIndices(LI, EndPoints) will reconstruct the
  /// value's live range.
  ///
  /// Calling pruneValue() and extendToIndices() can be used to reconstruct
  /// SSA form after adding defs to a virtual register.
  void pruneValue(CgLiveRange &LR, CgSlotIndex Kill,
                  SmallVectorImpl<CgSlotIndex> *EndPoints);

  /// This function should not be used. Its intent is to tell you that you are
  /// doing something wrong if you call pruneValue directly on a
  /// CgLiveInterval. Indeed, you are supposed to call pruneValue on the main
  /// CgLiveRange and all the LiveRanges of the subranges if any.
  LLVM_ATTRIBUTE_UNUSED void pruneValue(CgLiveInterval &, CgSlotIndex,
                                        SmallVectorImpl<CgSlotIndex> *) {
    llvm_unreachable(
        "Use pruneValue on the main CgLiveRange and on each subrange");
  }

  CgSlotIndexes *getSlotIndexes() const { return Indexes; }

  /// Returns true if the specified machine instr has been removed or was
  /// never entered in the map.
  bool isNotInMIMap(const CgInstruction &Instr) const {
    return !Indexes->hasIndex(Instr);
  }

  /// Returns the base index of the given instruction.
  CgSlotIndex getInstructionIndex(const CgInstruction &Instr) const {
    return Indexes->getInstructionIndex(Instr);
  }

  /// Returns the instruction associated with the given index.
  CgInstruction *getInstructionFromIndex(CgSlotIndex index) const {
    return Indexes->getInstructionFromIndex(index);
  }

  /// Return the first index in the given basic block.
  CgSlotIndex getMBBStartIdx(const CgBasicBlock *mbb) const {
    return Indexes->getMBBStartIdx(mbb);
  }

  /// Return the last index in the given basic block.
  CgSlotIndex getMBBEndIdx(const CgBasicBlock *mbb) const {
    return Indexes->getMBBEndIdx(mbb);
  }

  bool isLiveInToMBB(const CgLiveRange &LR, const CgBasicBlock *mbb) const {
    return LR.liveAt(getMBBStartIdx(mbb));
  }

  bool isLiveOutOfMBB(const CgLiveRange &LR, const CgBasicBlock *mbb) const {
    return LR.liveAt(getMBBEndIdx(mbb).getPrevSlot());
  }

  CgBasicBlock *getMBBFromIndex(CgSlotIndex index) const {
    return Indexes->getMBBFromIndex(index);
  }

  void insertMBBInMaps(CgBasicBlock *MBB) {
    Indexes->insertMBBInMaps(MBB);
    assert(unsigned(MBB->getNumber()) == RegMaskBlocks.size() &&
           "Blocks must be added in order.");
    RegMaskBlocks.push_back(std::make_pair(RegMaskSlots.size(), 0));
  }

  CgSlotIndex InsertMachineInstrInMaps(CgInstruction &MI) {
    return Indexes->insertCgInstructionInMaps(MI);
  }

  void InsertMachineInstrRangeInMaps(CgBasicBlock::iterator B,
                                     CgBasicBlock::iterator E) {
    for (CgBasicBlock::iterator I = B; I != E; ++I)
      Indexes->insertCgInstructionInMaps(*I);
  }

  void RemoveCgInstructionFromMaps(CgInstruction &MI) {
    Indexes->removeCgInstructionFromMaps(MI);
  }

  CgSlotIndex ReplaceMachineInstrInMaps(CgInstruction &MI,
                                        CgInstruction &NewMI) {
    return Indexes->replaceCgInstructionInMaps(MI, NewMI);
  }

  CgVNInfo::Allocator &getVNInfoAllocator() { return VNInfoAllocator; }

#if 0
    void getAnalysisUsage(AnalysisUsage &AU) const override;
#endif

  void releaseMemory();

  /// Pass entry point; Calculates CgLiveIntervals.
  bool runOnCgFunction(CgFunction &);

  /// Implement the dump method.
  void print(raw_ostream &O) const;
  void dump() const;

  /// If LI is confined to a single basic block, return a pointer to that
  /// block.  If LI is live in to or out of any block, return NULL.
  CgBasicBlock *intervalIsInOneMBB(const CgLiveInterval &LI) const;

  /// Returns true if VNI is killed by any PHI-def values in LI.
  /// This may conservatively return true to avoid expensive computations.
  bool hasPHIKill(const CgLiveInterval &LI, const CgVNInfo *VNI) const;

  /// Add kill flags to any instruction that kills a virtual register.
  void addKillFlags(const CgVirtRegMap *);

#if 0
    /// Call this method to notify CgLiveIntervals that instruction \p MI has been
    /// moved within a basic block. This will update the live intervals for all
    /// operands of \p MI. Moves between basic blocks are not supported.
    ///
    /// \param UpdateFlags Update live intervals for nonallocatable physregs.
    void handleMove(CgInstruction &MI, bool UpdateFlags = false);

    /// Update intervals of operands of all instructions in the newly
    /// created bundle specified by \p BundleStart.
    ///
    /// \param UpdateFlags Update live intervals for nonallocatable physregs.
    ///
    /// Assumes existing liveness is accurate.
    /// \pre BundleStart should be the first instruction in the Bundle.
    /// \pre BundleStart should not have a have CgSlotIndex as one will be
    /// assigned.
    void handleMoveIntoNewBundle(CgInstruction &BundleStart,
                                 bool UpdateFlags = false);
#endif

  /// Update live intervals for instructions in a range of iterators. It is
  /// intended for use after target hooks that may insert or remove
  /// instructions, and is only efficient for a small number of instructions.
  ///
  /// OrigRegs is a vector of registers that were originally used by the
  /// instructions in the range between the two iterators.
  ///
  /// Currently, the only only changes that are supported are simple removal
  /// and addition of uses.
  void repairIntervalsInRange(CgBasicBlock *MBB, CgBasicBlock::iterator Begin,
                              CgBasicBlock::iterator End,
                              ArrayRef<Register> OrigRegs);

  // Register mask functions.
  //
  // Machine instructions may use a register mask operand to indicate that a
  // large number of registers are clobbered by the instruction.  This is
  // typically used for calls.
  //
  // For compile time performance reasons, these clobbers are not recorded in
  // the live intervals for individual physical registers.  Instead,
  // LiveIntervalAnalysis maintains a sorted list of instructions with
  // register mask operands.

  /// Returns a sorted array of slot indices of all instructions with
  /// register mask operands.
  ArrayRef<CgSlotIndex> getRegMaskSlots() const { return RegMaskSlots; }

  /// Returns a sorted array of slot indices of all instructions with register
  /// mask operands in the basic block numbered \p MBBNum.
  ArrayRef<CgSlotIndex> getRegMaskSlotsInBlock(unsigned MBBNum) const {
    std::pair<unsigned, unsigned> P = RegMaskBlocks[MBBNum];
    return getRegMaskSlots().slice(P.first, P.second);
  }

  /// Returns an array of register mask pointers corresponding to
  /// getRegMaskSlots().
  ArrayRef<const uint32_t *> getRegMaskBits() const { return RegMaskBits; }

  /// Returns an array of mask pointers corresponding to
  /// getRegMaskSlotsInBlock(MBBNum).
  ArrayRef<const uint32_t *> getRegMaskBitsInBlock(unsigned MBBNum) const {
    std::pair<unsigned, unsigned> P = RegMaskBlocks[MBBNum];
    return getRegMaskBits().slice(P.first, P.second);
  }

  /// Test if \p LI is live across any register mask instructions, and
  /// compute a bit mask of physical registers that are not clobbered by any
  /// of them.
  ///
  /// Returns false if \p LI doesn't cross any register mask instructions. In
  /// that case, the bit vector is not filled in.
  bool checkRegMaskInterference(const CgLiveInterval &LI,
                                BitVector &UsableRegs);

  // Register unit functions.
  //
  // Fixed interference occurs when MachineInstrs use physregs directly
  // instead of virtual registers. This typically happens when passing
  // arguments to a function call, or when instructions require operands in
  // fixed registers.
  //
  // Each physreg has one or more register units, see MCRegisterInfo. We
  // track liveness per register unit to handle aliasing registers more
  // efficiently.

  /// Return the live range for register unit \p Unit. It will be computed if
  /// it doesn't exist.
  CgLiveRange &getRegUnit(unsigned Unit) {
    CgLiveRange *LR = RegUnitRanges[Unit];
    if (!LR) {
      // Compute missing ranges on demand.
      // Use segment set to speed-up initial computation of the live
      // range.
      RegUnitRanges[Unit] = LR =
          newObject<CgLiveRange>(UseSegmentSetForPhysRegs);
      computeRegUnitRange(*LR, Unit);
    }
    return *LR;
  }

  /// Return the live range for register unit \p Unit if it has already been
  /// computed, or nullptr if it hasn't been computed yet.
  CgLiveRange *getCachedRegUnit(unsigned Unit) { return RegUnitRanges[Unit]; }

  const CgLiveRange *getCachedRegUnit(unsigned Unit) const {
    return RegUnitRanges[Unit];
  }

  /// Remove computed live range for register unit \p Unit. Subsequent uses
  /// should rely on on-demand recomputation.
  void removeRegUnit(unsigned Unit) {
    deleteObject(RegUnitRanges[Unit]);
    RegUnitRanges[Unit] = nullptr;
  }

  /// Remove associated live ranges for the register units associated with \p
  /// Reg. Subsequent uses should rely on on-demand recomputation.  \note This
  /// method can result in inconsistent liveness tracking if multiple phyical
  /// registers share a regunit, and should be used cautiously.
  void removeAllRegUnitsForPhysReg(MCRegister Reg) {
    for (MCRegUnitIterator Units(Reg, TRI); Units.isValid(); ++Units)
      removeRegUnit(*Units);
  }

  /// Remove value numbers and related live segments starting at position
  /// \p Pos that are part of any liverange of physical register \p Reg or one
  /// of its subregisters.
  void removePhysRegDefAt(MCRegister Reg, CgSlotIndex Pos);

  /// Remove value number and related live segments of \p LI and its subranges
  /// that start at position \p Pos.
  void removeVRegDefAt(CgLiveInterval &LI, CgSlotIndex Pos);

  /// Split separate components in CgLiveInterval \p LI into separate
  /// intervals.
  void splitSeparateComponents(CgLiveInterval &LI,
                               SmallVectorImpl<CgLiveInterval *> &SplitLIs);

  /// For live interval \p LI with correct SubRanges construct matching
  /// information for the main live range. Expects the main live range to not
  /// have any segments or value numbers.
  void constructMainRangeFromSubranges(CgLiveInterval &LI);

private:
  /// Compute live intervals for all virtual registers.
  void computeVirtRegs();

  /// Compute RegMaskSlots and RegMaskBits.
  void computeRegMasks();

  /// Walk the values in \p LI and check for dead values:
  /// - Dead PHIDef values are marked as unused.
  /// - Dead operands are marked as such.
  /// - Completely dead machine instructions are added to the \p dead vector
  ///   if it is not nullptr.
  /// Returns true if any PHI value numbers have been removed which may
  /// have separated the interval into multiple connected components.
  bool computeDeadValues(CgLiveInterval &LI,
                         SmallVectorImpl<CgInstruction *> *dead);

  CgLiveInterval *createInterval(Register Reg);

  void printInstrs(raw_ostream &O) const;
  void dumpInstrs() const;

  void computeLiveInRegUnits();
  void computeRegUnitRange(CgLiveRange &, unsigned Unit);
  bool computeVirtRegInterval(CgLiveInterval &);

  using ShrinkToUsesWorkList =
      SmallVector<std::pair<CgSlotIndex, CgVNInfo *>, 16>;
  void extendSegmentsToUses(CgLiveRange &Segments,
                            ShrinkToUsesWorkList &WorkList, Register Reg,
                            LaneBitmask LaneMask);

  /// Helper function for repairIntervalsInRange(), walks backwards and
  /// creates/modifies live segments in \p LR to match the operands found.
  /// Only full operands or operands with subregisters matching \p LaneMask
  /// are considered.
  void repairOldRegInRange(CgBasicBlock::iterator Begin,
                           CgBasicBlock::iterator End, const CgSlotIndex endIdx,
                           CgLiveRange &LR, Register Reg,
                           LaneBitmask LaneMask = LaneBitmask::getAll());

  class HMEditor;
};

} // namespace COMPILER
