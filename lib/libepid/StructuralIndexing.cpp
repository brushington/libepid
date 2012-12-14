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
// The basic design is outlined in the header file for this pass. Further
// discussion here only covers low level design points.
//  * We use 32-bit IDs for all statements and functions within the program.
//  * To disambiguate function IDs from statement IDs, we use the most
//    significant bit of the ID as an "isFunction" flag. This allows us to
//    simply count the functions and basic blocks as we process them to
//    compute IDs.
//  * Static instruction IDs are also 32-bits and are computed on demand when
//    a client Pass requests a reified ExecutionPoint for a given Instruction
//    Collision between these IDs and the basic block or function IDs is
//    strictly safe.
//
//////////////////////////////////////////////////////////////////////////////

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"

#include "libepid/StructuralIndexing.h"


using namespace llvm;
using namespace libepid;


bool
StructuralIndexingPass::doInitialization(Module &m) {
  // Use 64-bit integers for the stack and loop counters and smaller words 
  // where we can get away with it.
  wideTy = IntegerType::get(m.getContext(), 64);
  idTy = IntegerType::get(m.getContext(), 32);

  const size_t BUFFER_SIZE = 0x100000;
  auto *bufferTy = ArrayType::get(wideTy, BUFFER_SIZE);
  idStack = new GlobalVariable(m, bufferTy, false,
                               GlobalValue::CommonLinkage,
                               ConstantAggregateZero::get(bufferTy),
                               "SEI.indexing_stack_buffer");

  Constant *zero = ConstantInt::getNullValue(idTy);
  Constant *gepInfo[] = { zero,  zero };
  auto startPtr = ConstantExpr::getGetElementPtr(idStack,gepInfo);
  stackTop = new GlobalVariable(m, wideTy->getPointerTo(), false,
                                 GlobalValue::CommonLinkage,
                                 startPtr,
                                 "SEI.indexing_stack_top");

  // Create the index push function
  Type *pushArgs[] = { idTy, idTy };
  auto *pushTy = FunctionType::get(Type::getVoidTy(m.getContext()),
                                   pushArgs, false);
  Function *push = Function::Create(pushTy, GlobalValue::CommonLinkage,
                                    "SEI.push_indexing_stack", &m); 
  {
    auto args = push->arg_begin();
    Value* pushID = args++;
    pushID->setName("pushID");
    Value* popID = args++;
    popID->setName("popID");

    auto* body = BasicBlock::Create(m.getContext(), "", push, 0);

    // Make the combined entry: (push << 32) | pop
    auto *pushID64 = new ZExtInst(pushID, wideTy, "", body);
    auto *const32 = ConstantInt::get(wideTy, 32);
    auto *shifted =
      BinaryOperator::Create(Instruction::Shl, pushID64, const32, "", body);
    auto *pop64 = new ZExtInst(popID, wideTy, "", body);
    auto *entry =
       BinaryOperator::Create(Instruction::Or, shifted, pop64, "", body);

    // Add it to the stack
    auto *topPtr = new LoadInst(stackTop, "", false, body);
    new StoreInst(entry, topPtr, false, body);
    auto *one = ConstantInt::get(wideTy, 1);
    auto *inc = GetElementPtrInst::Create(topPtr, one, "", body);
    new StoreInst(inc, stackTop, false, body);
    ReturnInst::Create(m.getContext(), body);
  }
  pushFun = push;

  // Create the index pop function
  auto *popTy = FunctionType::get(Type::getVoidTy(m.getContext()),
                                  idTy, false);
  Function *pop = Function::Create(popTy, GlobalValue::CommonLinkage,
                                    "SEI.pop_indexing_stack", &m); 
  {
    Value* popID = pop->arg_begin();
    popID->setName("popID");


    auto* pre = BasicBlock::Create(m.getContext(), "", pop, 0);
    auto* loop = BasicBlock::Create(m.getContext(), "", pop, 0);
    auto* post = BasicBlock::Create(m.getContext(), "", pop, 0);

    auto *topPtr = new LoadInst(stackTop, "", false, pre);
    BranchInst::Create(loop, pre);

    // Body
    Type *widePtr = wideTy->getPointerTo();
    Argument* forward = new Argument(widePtr);
    auto* phi = PHINode::Create(widePtr, 2, ".pn", loop);
    phi->addIncoming(topPtr, pre);
    phi->addIncoming(forward, loop);

    auto *negOne = ConstantInt::getSigned(idTy, -1);
    auto *gep = GetElementPtrInst::Create(phi, negOne, "current.0", loop);
    auto *top = new LoadInst(gep, "", false, loop);
    auto *trunc = new TruncInst(top, idTy, "", loop);
    auto *cmp = new ICmpInst(*loop, ICmpInst::ICMP_EQ, trunc, popID, "");
    BranchInst::Create(loop, post, cmp, loop);

    // Post
    new StoreInst(phi, stackTop, false, post);
    ReturnInst::Create(m.getContext(), post);

    // Resolve Forward References
    forward->replaceAllUsesWith(gep);
    delete forward;
  }
  popFun = pop;

  unsigned fid = 0x80000000;
  for (auto &f : m) {
    functionIDs[&f] = fid++;
  }

  return true;
}


void
StructuralIndexingPass::computeControlDependence(Function &f,
                                                 ControlDepMap &controlDeps) {
  auto &postTree = getAnalysis<PostDominatorTree>();

  auto run = [&postTree, &controlDeps] (BasicBlock *path,
                                        BasicBlock *branch,
                                        BasicBlock *post) {
    while (path && path != post) {
      controlDeps[path].insert(branch);
      unsigned numSuccessors = path->getTerminator()->getNumSuccessors();
      if (1 == numSuccessors) {
        path = *succ_begin(path);
      } else if (numSuccessors > 1) {
        path = postTree[path]->getIDom()->getBlock();
      } else {
        break;
      }
    }
  };

  for (auto &bb : f) {
    if (bb.getTerminator()->getNumSuccessors() <= 1) {
      continue;
    }

    BasicBlock *post = postTree[&bb]->getIDom()->getBlock();
    for (auto s = succ_begin(&bb), e = succ_end(&bb); s != e; ++s) {
      run(*s, &bb, post);
    }
  }
}


typedef DenseMap<Loop*, SmallPtrSet<BasicBlock*,4>> LoopInfoMap;

static void
collectLoopExits(Loop &loop, LoopInfoMap &loopInfo) {
  SmallVector<BasicBlock*,4> exits;
  loop.getExitingBlocks(exits);
  auto &info = loopInfo[&loop];
  for (auto &exit : exits) {
    info.insert(exit);
  }

  for (Loop *inner : loop) {
    collectLoopExits(*inner, loopInfo);
  }
}


static BasicBlock *
findReturnBlock(Function &f) {
  for (auto &bb : f) {
    if (isa<ReturnInst>(bb.getTerminator())) {
      return &bb;
    }
  }
  return nullptr;
}


bool
StructuralIndexingPass::runOnFunction(Function &f) {
  ControlDepMap controlDeps;
  computeControlDependence(f, controlDeps);

  DenseSet<BasicBlock*> pushes;

  for (auto &bb : f) {
    auto &deps = controlDeps[&bb];
    if (deps.size() <= 1) {
      continue;
    }
    for (auto branch : deps) {
      pushes.insert(branch);
    }
  }

  LoopInfoMap loopInfo;
  for  (auto &loop : getAnalysis<LoopInfo>()) {
    collectLoopExits(*loop, loopInfo);
  }
  for (auto &exitPair : loopInfo) {
    for (auto *branch : exitPair.second) {
      pushes.insert(branch);
    }
  }

  auto *returnBlock = findReturnBlock(f);
  auto &postTree = getAnalysis<PostDominatorTree>();
  auto getPostOrReturn =
    [&postTree, returnBlock] (BasicBlock *bb) -> BasicBlock * {
    auto *post = postTree[bb]->getIDom()->getBlock();
    if (!post) {
      post = returnBlock;
    }
    return post;
  };

  DenseSet<BasicBlock*> pops;
  for (auto push : pushes) {
    pops.insert(getPostOrReturn(push));
  }

  unsigned id = 0;
  DenseMap<BasicBlock*,unsigned> blockIDs;
  for (auto &bb : f) {
    blockIDs[&bb] = ++id;
  }

  unsigned numCalls = 0;
  unsigned fID = functionIDs[&f];
  for (auto &bb : f) {
    SmallVector<CallSite,8> calls;
    for (auto &i : bb) {
      CallSite cs(&i);
      if (cs.getInstruction()) {
        calls.push_back(cs);
      }
    }

    if (pushes.count(&bb)) {
      auto *post = getPostOrReturn(&bb);
      insertPush(bb.getTerminator(), blockIDs[&bb], blockIDs[post]);
    }
    if (pops.count(&bb)) {
      insertPop(bb.getFirstNonPHI(), blockIDs[&bb]);
    }

    for (auto cs : calls) {
      insertPush(cs.getInstruction(), ++numCalls, fID);
      // TODO handle exceptions
      insertPop(++BasicBlock::iterator(cs.getInstruction()), fID);
    }
  }

  return true;
}


void
StructuralIndexingPass::insertPush(Instruction *i,
                                   unsigned push,
                                   unsigned pop) {
  Value *args[] = {
    ConstantInt::get(idTy, push),
    ConstantInt::get(idTy, pop)
  };
  CallInst::Create(pushFun, args, "", i);
}


void
StructuralIndexingPass::insertPop(Instruction *i, unsigned pop) {
  CallInst::Create(popFun, ConstantInt::get(idTy, pop), "", i);
}


bool
StructuralIndexingPass::doFinalization(Module &m) {
  return false;
}


ExecutionPoint
StructuralIndexingPass::createCurrentPoint(Instruction *pos) {
  // TODO
  return ExecutionPoint{nullptr, nullptr};
}


char StructuralIndexingPass::ID = 0;
RegisterPass<StructuralIndexingPass> X("seindexing", "Structural Execution Indexing Pass");
const PassInfo *const StructuralIndexingPassID = &X;

