//===- llvm/CodeGen/CgSlotIndexes.h - Slot indexes representation -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements CgSlotIndex and related classes. The purpose of
// CgSlotIndex is to describe a position at which a register can become live, or
// cease to be live.
//
// CgSlotIndex is mostly a proxy for entries of the SlotIndexList, a class which
// is held is CgLiveIntervals and provides the real numbering. This allows
// CgLiveIntervals to perform largely transparent renumbering.
//===----------------------------------------------------------------------===//
#pragma once

#include "compiler/cgir/cg_basic_block.h"
#include "compiler/cgir/cg_instruction.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/ilist.h"
#include "llvm/Support/Allocator.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <utility>

using namespace llvm;

namespace COMPILER {
class IndexListEntry;
class CgSlotIndex;
class CgFunction;
} // namespace COMPILER

namespace llvm {

class raw_ostream;
template <>
struct ilist_alloc_traits<COMPILER::IndexListEntry>
    : public ilist_noalloc_traits<COMPILER::IndexListEntry> {};

// Specialize IntervalMapInfo for half-open slot index intervals.
template <>
struct IntervalMapInfo<CgSlotIndex> : IntervalMapHalfOpenInfo<CgSlotIndex> {};

} // end namespace llvm

namespace COMPILER {

/// This class represents an entry in the slot index list held in the
/// CgSlotIndexes pass. It should not be used directly. See the
/// CgSlotIndex & CgSlotIndexes classes for the public interface to this
/// information.
class IndexListEntry : public ilist_node<IndexListEntry> {
  CgInstruction *mi;
  unsigned index;

public:
  IndexListEntry(CgInstruction *mi, unsigned index) : mi(mi), index(index) {}

  CgInstruction *getInstr() const { return mi; }
  void setInstr(CgInstruction *mi) { this->mi = mi; }

  unsigned getIndex() const { return index; }
  void setIndex(unsigned index) { this->index = index; }

#ifdef EXPENSIVE_CHECKS
  // When EXPENSIVE_CHECKS is defined, "erased" index list entries will
  // actually be moved to a "graveyard" list, and have their pointers
  // poisoned, so that dangling CgSlotIndex access can be reliably detected.
  void setPoison() {
    intptr_t tmp = reinterpret_cast<intptr_t>(mi);
    assert(((tmp & 0x1) == 0x0) && "Pointer already poisoned?");
    tmp |= 0x1;
    mi = reinterpret_cast<CgInstruction *>(tmp);
  }

  bool isPoisoned() const {
    return (reinterpret_cast<intptr_t>(mi) & 0x1) == 0x1;
  }
#endif // EXPENSIVE_CHECKS
};

/// CgSlotIndex - An opaque wrapper around machine indexes.
class CgSlotIndex {
  friend class CgSlotIndexes;

  enum Slot {
    /// Basic block boundary.  Used for live ranges entering and leaving a
    /// block without being live in the layout neighbor.  Also used as the
    /// def slot of PHI-defs.
    Slot_Block,

    /// Early-clobber register use/def slot.  A live range defined at
    /// Slot_EarlyClobber interferes with normal live ranges killed at
    /// Slot_Register.  Also used as the kill slot for live ranges tied to
    /// an
    /// early-clobber def.
    Slot_EarlyClobber,

    /// Normal register use/def slot.  Normal instructions kill and define
    /// register live ranges at this slot.
    Slot_Register,

    /// Dead def kill point.  Kill slot for a live range that is defined by
    /// the same instruction (Slot_Register or Slot_EarlyClobber), but isn't
    /// used anywhere.
    Slot_Dead,

    Slot_Count
  };

  PointerIntPair<IndexListEntry *, 2, unsigned> lie;

  CgSlotIndex(IndexListEntry *entry, unsigned slot) : lie(entry, slot) {}

  IndexListEntry *listEntry() const {
    assert(isValid() && "Attempt to compare reserved index.");
#ifdef EXPENSIVE_CHECKS
    assert(!lie.getPointer()->isPoisoned() &&
           "Attempt to access deleted list-entry.");
#endif // EXPENSIVE_CHECKS
    return lie.getPointer();
  }

  unsigned getIndex() const { return listEntry()->getIndex() | getSlot(); }

  /// Returns the slot for this CgSlotIndex.
  Slot getSlot() const { return static_cast<Slot>(lie.getInt()); }

public:
  enum {
    /// The default distance between instructions as returned by distance().
    /// This may vary as instructions are inserted and removed.
    InstrDist = 4 * Slot_Count
  };

  /// Construct an invalid index.
  CgSlotIndex() = default;

  // Construct a new slot index from the given one, and set the slot.
  CgSlotIndex(const CgSlotIndex &li, Slot s)
      : lie(li.listEntry(), unsigned(s)) {
    assert(lie.getPointer() != nullptr &&
           "Attempt to construct index with 0 pointer.");
  }

  /// Returns true if this is a valid index. Invalid indices do
  /// not point into an index table, and cannot be compared.
  bool isValid() const { return lie.getPointer(); }

  /// Return true for a valid index.
  explicit operator bool() const { return isValid(); }

  /// Print this index to the given raw_ostream.
  void print(raw_ostream &os) const;

  /// Dump this index to stderr.
  void dump() const;

  /// Compare two CgSlotIndex objects for equality.
  bool operator==(CgSlotIndex other) const { return lie == other.lie; }
  /// Compare two CgSlotIndex objects for inequality.
  bool operator!=(CgSlotIndex other) const { return lie != other.lie; }

  /// Compare two CgSlotIndex objects. Return true if the first index
  /// is strictly lower than the second.
  bool operator<(CgSlotIndex other) const {
    return getIndex() < other.getIndex();
  }
  /// Compare two CgSlotIndex objects. Return true if the first index
  /// is lower than, or equal to, the second.
  bool operator<=(CgSlotIndex other) const {
    return getIndex() <= other.getIndex();
  }

  /// Compare two CgSlotIndex objects. Return true if the first index
  /// is greater than the second.
  bool operator>(CgSlotIndex other) const {
    return getIndex() > other.getIndex();
  }

  /// Compare two CgSlotIndex objects. Return true if the first index
  /// is greater than, or equal to, the second.
  bool operator>=(CgSlotIndex other) const {
    return getIndex() >= other.getIndex();
  }

  /// isSameInstr - Return true if A and B refer to the same instruction.
  static bool isSameInstr(CgSlotIndex A, CgSlotIndex B) {
    return A.lie.getPointer() == B.lie.getPointer();
  }

  /// isEarlierInstr - Return true if A refers to an instruction earlier than
  /// B. This is equivalent to A < B && !isSameInstr(A, B).
  static bool isEarlierInstr(CgSlotIndex A, CgSlotIndex B) {
    return A.listEntry()->getIndex() < B.listEntry()->getIndex();
  }

  /// Return true if A refers to the same instruction as B or an earlier one.
  /// This is equivalent to !isEarlierInstr(B, A).
  static bool isEarlierEqualInstr(CgSlotIndex A, CgSlotIndex B) {
    return !isEarlierInstr(B, A);
  }

  /// Return the distance from this index to the given one.
  int distance(CgSlotIndex other) const {
    return other.getIndex() - getIndex();
  }

  /// Return the scaled distance from this index to the given one, where all
  /// slots on the same instruction have zero distance.
  int getInstrDistance(CgSlotIndex other) const {
    return (other.listEntry()->getIndex() - listEntry()->getIndex()) /
           Slot_Count;
  }

  /// isBlock - Returns true if this is a block boundary slot.
  bool isBlock() const { return getSlot() == Slot_Block; }

  /// isEarlyClobber - Returns true if this is an early-clobber slot.
  bool isEarlyClobber() const { return getSlot() == Slot_EarlyClobber; }

  /// isRegister - Returns true if this is a normal register use/def slot.
  /// Note that early-clobber slots may also be used for uses and defs.
  bool isRegister() const { return getSlot() == Slot_Register; }

  /// isDead - Returns true if this is a dead def kill slot.
  bool isDead() const { return getSlot() == Slot_Dead; }

  /// Returns the base index for associated with this index. The base index
  /// is the one associated with the Slot_Block slot for the instruction
  /// pointed to by this index.
  CgSlotIndex getBaseIndex() const {
    return CgSlotIndex(listEntry(), Slot_Block);
  }

  /// Returns the boundary index for associated with this index. The boundary
  /// index is the one associated with the Slot_Block slot for the instruction
  /// pointed to by this index.
  CgSlotIndex getBoundaryIndex() const {
    return CgSlotIndex(listEntry(), Slot_Dead);
  }

  /// Returns the register use/def slot in the current instruction for a
  /// normal or early-clobber def.
  CgSlotIndex getRegSlot(bool EC = false) const {
    return CgSlotIndex(listEntry(), EC ? Slot_EarlyClobber : Slot_Register);
  }

  /// Returns the dead def kill slot for the current instruction.
  CgSlotIndex getDeadSlot() const {
    return CgSlotIndex(listEntry(), Slot_Dead);
  }

  /// Returns the next slot in the index list. This could be either the
  /// next slot for the instruction pointed to by this index or, if this
  /// index is a STORE, the first slot for the next instruction.
  /// WARNING: This method is considerably more expensive than the methods
  /// that return specific slots (getUseIndex(), etc). If you can - please
  /// use one of those methods.
  CgSlotIndex getNextSlot() const {
    Slot s = getSlot();
    if (s == Slot_Dead) {
      return CgSlotIndex(&*++listEntry()->getIterator(), Slot_Block);
    }
    return CgSlotIndex(listEntry(), s + 1);
  }

  /// Returns the next index. This is the index corresponding to the this
  /// index's slot, but for the next instruction.
  CgSlotIndex getNextIndex() const {
    return CgSlotIndex(&*++listEntry()->getIterator(), getSlot());
  }

  /// Returns the previous slot in the index list. This could be either the
  /// previous slot for the instruction pointed to by this index or, if this
  /// index is a Slot_Block, the last slot for the previous instruction.
  /// WARNING: This method is considerably more expensive than the methods
  /// that return specific slots (getUseIndex(), etc). If you can - please
  /// use one of those methods.
  CgSlotIndex getPrevSlot() const {
    Slot s = getSlot();
    if (s == Slot_Block) {
      return CgSlotIndex(&*--listEntry()->getIterator(), Slot_Dead);
    }
    return CgSlotIndex(listEntry(), s - 1);
  }

  /// Returns the previous index. This is the index corresponding to this
  /// index's slot, but for the previous instruction.
  CgSlotIndex getPrevIndex() const {
    return CgSlotIndex(&*--listEntry()->getIterator(), getSlot());
  }
};

inline raw_ostream &operator<<(raw_ostream &os, CgSlotIndex li) {
  li.print(os);
  return os;
}

using IdxMBBPair = std::pair<CgSlotIndex, CgBasicBlock *>;

/// CgSlotIndexes pass.
///
/// This pass assigns indexes to each instruction.
class CgSlotIndexes : public NonCopyable {
private:
  // IndexListEntry allocator.
  BumpPtrAllocator ileAllocator;

  using IndexList = ilist<IndexListEntry>;
  IndexList indexList;

  CgFunction *mf = nullptr;

  using Mi2IndexMap = DenseMap<const CgInstruction *, CgSlotIndex>;
  Mi2IndexMap mi2iMap;

  /// MBBRanges - Map MBB number to (start, stop) indexes.
  SmallVector<std::pair<CgSlotIndex, CgSlotIndex>, 8> MBBRanges;

  /// Idx2MBBMap - Sorted list of pairs of index of first instruction
  /// and MBB id.
  SmallVector<IdxMBBPair, 8> idx2MBBMap;

  IndexListEntry *createEntry(CgInstruction *mi, unsigned index) {
    IndexListEntry *entry = static_cast<IndexListEntry *>(
        ileAllocator.Allocate(sizeof(IndexListEntry), alignof(IndexListEntry)));

    new (entry) IndexListEntry(mi, index);

    return entry;
  }

  /// Renumber locally after inserting curItr.
  void renumberIndexes(IndexList::iterator curItr);

public:
  // static char ID;

  CgSlotIndexes(CgFunction &MF);
  ~CgSlotIndexes();
#if 0
    void getAnalysisUsage(AnalysisUsage &au) const override;
#endif
  void releaseMemory();
  void runOnCgFunction(CgFunction &fn);

  /// Dump the indexes.
  void dump() const;

  /// Repair indexes after adding and removing instructions.
  void repairIndexesInRange(CgBasicBlock *MBB, CgBasicBlock::iterator Begin,
                            CgBasicBlock::iterator End);

  /// Returns the zero index for this analysis.
  CgSlotIndex getZeroIndex() {
    assert(indexList.front().getIndex() == 0 && "First index is not 0?");
    return CgSlotIndex(&indexList.front(), 0);
  }

  /// Returns the base index of the last slot in this analysis.
  CgSlotIndex getLastIndex() { return CgSlotIndex(&indexList.back(), 0); }

  /// Returns true if the given machine instr is mapped to an index,
  /// otherwise returns false.
  bool hasIndex(const CgInstruction &instr) const {
    return mi2iMap.count(&instr);
  }

  /// Returns the base index for the given instruction.
  CgSlotIndex getInstructionIndex(const CgInstruction &MI,
                                  bool IgnoreBundle = false) const {
#if 0
      // Instructions inside a bundle have the same number as the bundle itself.
      auto BundleStart = getBundleStart(MI.getIterator());
      auto BundleEnd = getBundleEnd(MI.getIterator());
      // Use the first non-debug instruction in the bundle to get CgSlotIndex.
      const CgInstruction &BundleNonDebug =
          IgnoreBundle ? MI
                       : *skipDebugInstructionsForward(BundleStart, BundleEnd);
      assert(!BundleNonDebug.isDebugInstr() &&
             "Could not use a debug instruction to query mi2iMap.");
      Mi2IndexMap::const_iterator itr = mi2iMap.find(&BundleNonDebug);
#endif

    Mi2IndexMap::const_iterator itr = mi2iMap.find(&MI);
    assert(itr != mi2iMap.end() && "Instruction not found in maps.");
    return itr->second;
  }

  /// Returns the instruction for the given index, or null if the given
  /// index has no instruction associated with it.
  CgInstruction *getInstructionFromIndex(CgSlotIndex index) const {
    return index.isValid() ? index.listEntry()->getInstr() : nullptr;
  }

  /// Returns the next non-null index, if one exists.
  /// Otherwise returns getLastIndex().
  CgSlotIndex getNextNonNullIndex(CgSlotIndex Index) {
    IndexList::iterator I = Index.listEntry()->getIterator();
    IndexList::iterator E = indexList.end();
    while (++I != E)
      if (I->getInstr())
        return CgSlotIndex(&*I, Index.getSlot());
    // We reached the end of the function.
    return getLastIndex();
  }

  /// getIndexBefore - Returns the index of the last indexed instruction
  /// before MI, or the start index of its basic block.
  /// MI is not required to have an index.
  CgSlotIndex getIndexBefore(const CgInstruction &MI) const {
    const CgBasicBlock *MBB = MI.getParent();
    assert(MBB && "MI must be inserted in a basic block");
    CgBasicBlock::const_iterator I = CgBasicBlock::const_iterator(MI),
                                 B = MBB->begin();
    while (true) {
      if (I == B)
        return getMBBStartIdx(MBB);
      --I;
      Mi2IndexMap::const_iterator MapItr = mi2iMap.find(&*I);
      if (MapItr != mi2iMap.end())
        return MapItr->second;
    }
  }

  /// getIndexAfter - Returns the index of the first indexed instruction
  /// after MI, or the end index of its basic block.
  /// MI is not required to have an index.
  CgSlotIndex getIndexAfter(const CgInstruction &MI) const {
    const CgBasicBlock *MBB = MI.getParent();
    assert(MBB && "MI must be inserted in a basic block");
    CgBasicBlock::const_iterator I = CgBasicBlock::const_iterator(MI),
                                 E = MBB->end();
    while (true) {
      ++I;
      if (I == E)
        return getMBBEndIdx(MBB);
      Mi2IndexMap::const_iterator MapItr = mi2iMap.find(&*I);
      if (MapItr != mi2iMap.end())
        return MapItr->second;
    }
  }

  /// Return the (start,end) range of the given basic block number.
  const std::pair<CgSlotIndex, CgSlotIndex> &getMBBRange(unsigned Num) const {
    return MBBRanges[Num];
  }

  /// Return the (start,end) range of the given basic block.
  const std::pair<CgSlotIndex, CgSlotIndex> &
  getMBBRange(const CgBasicBlock *MBB) const {
    return getMBBRange(MBB->getNumber());
  }

  /// Returns the first index in the given basic block number.
  CgSlotIndex getMBBStartIdx(unsigned Num) const {
    return getMBBRange(Num).first;
  }

  /// Returns the first index in the given basic block.
  CgSlotIndex getMBBStartIdx(const CgBasicBlock *mbb) const {
    return getMBBRange(mbb).first;
  }

  /// Returns the last index in the given basic block number.
  CgSlotIndex getMBBEndIdx(unsigned Num) const {
    return getMBBRange(Num).second;
  }

  /// Returns the last index in the given basic block.
  CgSlotIndex getMBBEndIdx(const CgBasicBlock *mbb) const {
    return getMBBRange(mbb).second;
  }

  /// Iterator over the idx2MBBMap (sorted pairs of slot index of basic block
  /// begin and basic block)
  using MBBIndexIterator = SmallVectorImpl<IdxMBBPair>::const_iterator;

  /// Move iterator to the next IdxMBBPair where the CgSlotIndex is greater or
  /// equal to \p To.
  MBBIndexIterator advanceMBBIndex(MBBIndexIterator I, CgSlotIndex To) const {
    return std::partition_point(I, idx2MBBMap.end(), [=](const IdxMBBPair &IM) {
      return IM.first < To;
    });
  }

  /// Get an iterator pointing to the IdxMBBPair with the biggest CgSlotIndex
  /// that is greater or equal to \p Idx.
  MBBIndexIterator findMBBIndex(CgSlotIndex Idx) const {
    return advanceMBBIndex(idx2MBBMap.begin(), Idx);
  }

  /// Returns an iterator for the begin of the idx2MBBMap.
  MBBIndexIterator MBBIndexBegin() const { return idx2MBBMap.begin(); }

  /// Return an iterator for the end of the idx2MBBMap.
  MBBIndexIterator MBBIndexEnd() const { return idx2MBBMap.end(); }

  /// Returns the basic block which the given index falls in.
  CgBasicBlock *getMBBFromIndex(CgSlotIndex index) const {
    if (CgInstruction *MI = getInstructionFromIndex(index))
      return MI->getParent();

    MBBIndexIterator I = findMBBIndex(index);
    // Take the pair containing the index
    MBBIndexIterator J = ((I != MBBIndexEnd() && I->first > index) ||
                          (I == MBBIndexEnd() && !idx2MBBMap.empty()))
                             ? std::prev(I)
                             : I;

    assert(J != MBBIndexEnd() && J->first <= index &&
           index < getMBBEndIdx(J->second) &&
           "index does not correspond to an MBB");
    return J->second;
  }

  /// Insert the given machine instruction into the mapping. Returns the
  /// assigned index.
  /// If Late is set and there are null indexes between mi's neighboring
  /// instructions, create the new index after the null indexes instead of
  /// before them.
  CgSlotIndex insertCgInstructionInMaps(CgInstruction &MI, bool Late = false) {
    // assert(!MI.isInsideBundle() &&
    //        "Instructions inside bundles should use bundle start's
    //        slot.");
    assert(mi2iMap.find(&MI) == mi2iMap.end() && "Instr already indexed.");
    // Numbering debug instructions could cause code generation to be
    // affected by debug information.
    // assert(!MI.isDebugInstr() && "Cannot number debug instructions.");

    assert(MI.getParent() != nullptr && "Instr must be added to function.");

    // Get the entries where MI should be inserted.
    IndexList::iterator prevItr, nextItr;
    if (Late) {
      // Insert MI's index immediately before the following instruction.
      nextItr = getIndexAfter(MI).listEntry()->getIterator();
      prevItr = std::prev(nextItr);
    } else {
      // Insert MI's index immediately after the preceding instruction.
      prevItr = getIndexBefore(MI).listEntry()->getIterator();
      nextItr = std::next(prevItr);
    }

    // Get a number for the new instr, or 0 if there's no room currently.
    // In the latter case we'll force a renumber later.
    unsigned dist = ((nextItr->getIndex() - prevItr->getIndex()) / 2) & ~3u;
    unsigned newNumber = prevItr->getIndex() + dist;

    // Insert a new list entry for MI.
    IndexList::iterator newItr =
        indexList.insert(nextItr, createEntry(&MI, newNumber));

    // Renumber locally if we need to.
    if (dist == 0)
      renumberIndexes(newItr);

    CgSlotIndex newIndex(&*newItr, CgSlotIndex::Slot_Block);
    mi2iMap.insert(std::make_pair(&MI, newIndex));
    return newIndex;
  }

  /// Removes machine instruction (bundle) \p MI from the mapping.
  /// This should be called before CgInstruction::eraseFromParent() is used to
  /// remove a whole bundle or an unbundled instruction.
  /// If \p AllowBundled is set then this can be used on a bundled
  /// instruction; however, this exists to support handleMoveIntoBundle,
  /// and in general removeSingleCgInstructionFromMaps should be used instead.
  void removeCgInstructionFromMaps(CgInstruction &MI,
                                   bool AllowBundled = false);

  /// Removes a single machine instruction \p MI from the mapping.
  /// This should be called before CgInstruction::eraseFromBundle() is used to
  /// remove a single instruction (out of a bundle).
  void removeSingleCgInstructionFromMaps(CgInstruction &MI);

  /// ReplaceCgInstructionInMaps - Replacing a machine instr with a new one in
  /// maps used by register allocator. \returns the index where the new
  /// instruction was inserted.
  CgSlotIndex replaceCgInstructionInMaps(CgInstruction &MI,
                                         CgInstruction &NewMI) {
    Mi2IndexMap::iterator mi2iItr = mi2iMap.find(&MI);
    if (mi2iItr == mi2iMap.end())
      return CgSlotIndex();
    CgSlotIndex replaceBaseIndex = mi2iItr->second;
    IndexListEntry *miEntry(replaceBaseIndex.listEntry());
    assert(miEntry->getInstr() == &MI &&
           "Mismatched instruction in index tables.");
    miEntry->setInstr(&NewMI);
    mi2iMap.erase(mi2iItr);
    mi2iMap.insert(std::make_pair(&NewMI, replaceBaseIndex));
    return replaceBaseIndex;
  }

  /// Add the given CgBasicBlock into the maps.
  /// If it contains any instructions then they must already be in the maps.
  /// This is used after a block has been split by moving some suffix of its
  /// instructions into a newly created block.
  void insertMBBInMaps(CgBasicBlock *mbb) {
    ZEN_UNREACHABLE();
#if 0
      assert(mbb != &mbb->getParent()->front() &&
             "Can't insert a new block at the beginning of a function.");
      auto prevMBB = std::prev(CgFunction::iterator(mbb));
      // auto prevMBB = mm->getParent()->getCgBasicBlock(mbb->getNumber() - 1);

      // Create a new entry to be used for the start of mbb and the end of
      // prevMBB.
      IndexListEntry *startEntry = createEntry(nullptr, 0);
      IndexListEntry *endEntry = getMBBEndIdx(&*prevMBB).listEntry();
      IndexListEntry *insEntry =
          mbb->empty() ? endEntry
                       : getInstructionIndex(mbb->front()).listEntry();
      IndexList::iterator newItr =
          indexList.insert(insEntry->getIterator(), startEntry);

      CgSlotIndex startIdx(startEntry, CgSlotIndex::Slot_Block);
      CgSlotIndex endIdx(endEntry, CgSlotIndex::Slot_Block);

      MBBRanges[prevMBB->getNumber()].second = startIdx;

      assert(unsigned(mbb->getNumber()) == MBBRanges.size() &&
             "Blocks must be added in order");
      MBBRanges.push_back(std::make_pair(startIdx, endIdx));
      idx2MBBMap.push_back(IdxMBBPair(startIdx, mbb));

      renumberIndexes(newItr);
      llvm::sort(idx2MBBMap, less_first());
#endif
  }
};

} // end namespace COMPILER
