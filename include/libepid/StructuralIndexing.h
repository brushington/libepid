//////////////////////////////////////////////////////////////////////////////
//
// Libepid - the execution point identifier library
//
// This file is distributed under the MIT license.
// (http://opensource.org/licenses/MIT)
//
// The MIT License (MIT)
// 
// Copyright (c) 2012 William N. Sumner
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in 
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
// 
//////////////////////////////////////////////////////////////////////////////
//
// Structural execution indexing identifies execution points by their active
// control dependence. Efficiently computing and storing the control
// dependence along with the currently executing instruction allows the 
// technique to disambiguate different instructions.
// 
// The general strategy for computing the control dependence is:
// 1) In general, push/pop loops and function calls normally.
// 2) Infer unique, acyclic control dependences from the static positions.
// 3) For ambiguous, acyclic control depdences, use counters to disambiguate.
//    Dependences are ambiguous if one static statement is control dependent 
//    upon multiple static predicates.
// 4) Loops with a single controlling predicate can use a loop counter to
//    avoid pushing an ID for every iteration.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef LIBEPID_STRUCTURALINDEXING_H
#define LIBEPID_STRUCTURALINDEXING_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Pass.h"

#include "libepid/ExecutionPoint.h"


class llvm::Type;
class llvm::Constant;


namespace libepid {


// TODO - We could really do this with a FunctionPass instead.
//  That ought to be considered, along with any inconsistencies that might
//  create with other execution point systems.
struct StructuralIndexingPass
    : public llvm::FunctionPass, public ExecutionPointProvider {

  static char ID;

private:
  size_t pointCount;

  llvm::IntegerType *wideTy;
  llvm::IntegerType *idTy;
  llvm::Constant *idStack;
  llvm::Constant *stackTop;

  llvm::Function *popFun;
  llvm::Function *pushFun;

  llvm::DenseMap<llvm::Function*,unsigned> functionIDs;

  typedef llvm::DenseMap<llvm::BasicBlock*, 
                         llvm::SmallPtrSet<llvm::BasicBlock*,4>> ControlDepMap;
  void computeControlDependence(llvm::Function &f, ControlDepMap &deps);

  void insertPush(llvm::Instruction *i, unsigned pushID, unsigned popID);
  void insertPop(llvm::Instruction *i, unsigned popID);

public:

  StructuralIndexingPass() : FunctionPass(ID), pointCount(0) {}

  virtual bool doInitialization(llvm::Module &m) override;

  virtual bool runOnFunction(llvm::Function &f) override;

  virtual bool doFinalization(llvm::Module &m) override;

  virtual ExecutionPoint createCurrentPoint(llvm::Instruction *point) override;

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<llvm::UnifyFunctionExitNodes>();
    AU.addRequired<llvm::LoopInfo>();
    AU.addRequired<llvm::PostDominatorTree>();
  }
};


}


#endif

