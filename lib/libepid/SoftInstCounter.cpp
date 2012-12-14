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


#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"

#include "libepid/SoftInstCounter.h"


using namespace llvm;
using namespace libepid;


void
SoftInstCounterPass::incrementCounter(Instruction *pos) {
  auto load = new LoadInst(counter, "SIC.count", pos);
  auto one = ConstantInt::get(counterTy, 1);
  auto inc = BinaryOperator::Create(Instruction::Add, load, one, "", pos);
  new StoreInst(inc, counter, pos);
}


bool
SoftInstCounterPass::runOnModule(Module &m) {
  // Create the monotonic counter
  counterTy = IntegerType::get(m.getContext(), 64);
  counter = new GlobalVariable(m, counterTy, false,
                              GlobalValue::CommonLinkage,
                              Constant::getNullValue(counterTy),
                              "Software_Instruction_Counter.counter");

  auto pointInfoTy = ArrayType::get(counterTy, 2);
  pointBuffer = new GlobalVariable(m, pointInfoTy, false,
                                   GlobalValue::CommonLinkage,
                                   ConstantAggregateZero::get(pointInfoTy),
                                   "Software_Instruction_Counter.point");

  // We need to monotonically increase the counter at every possible
  // control flow backedge, including via function calls. For each function,
  // we conservatively do so at control flow backedges and all call sites.
  for (auto &f : m) {
    // First all call sites
    for (auto &bb : f) {
      for (auto &i : bb) {
        CallSite cs(&i);
        if (cs.getInstruction()) {
          incrementCounter(&i);
        }
      }
    }

    // Then all back edges
    SmallVector<std::pair<const BasicBlock*,const BasicBlock*>,8> backedges;
    FindFunctionBackedges(f, backedges);
    for (auto &back : backedges) {
      auto *from = const_cast<BasicBlock*>(back.first);
      incrementCounter(from->getTerminator());
    }
  }

  return true;
}


ExecutionPoint
SoftInstCounterPass::createCurrentPoint(Instruction *pos) {
  Constant *zero = ConstantInt::get(counterTy, 0);
  Value *gepInfo[] = { zero, zero };
  Value *first = GetElementPtrInst::Create(pointBuffer, gepInfo, "", pos);
  auto load = new LoadInst(counter, "SIC.localcount", pos);
  new StoreInst(load, first, pos);

  pointIDs[pos] = pointCount;
  Constant *stmtID = ConstantInt::get(counterTy, pointCount);
  ++pointCount;
  gepInfo[1] = ConstantInt::get(counterTy, 1);
  Value *second = GetElementPtrInst::Create(pointBuffer, gepInfo, "", pos);
  new StoreInst(stmtID, second, pos);

  auto point = new LoadInst(pointBuffer, "SIC.point", pos);
  auto size = ConstantInt::get(counterTy, 2 * counterTy->getBitWidth() / 8);
  return ExecutionPoint{point, size};
}


char SoftInstCounterPass::ID = 0;
RegisterPass<SoftInstCounterPass> X("softinstcount", "Software Instruction Counter Pass");
const PassInfo *const SoftInstCounterPassID = &X;

