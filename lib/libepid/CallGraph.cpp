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


#define DEBUG_TYPE "cgbuilder"

#include <vector>
#include <iostream>
#include <set>
#include <algorithm>

#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/User.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "libepid/CallGraph.h"
#include "util/CallSites.h"

STATISTIC(BackEdges, "Number of recursive calls");
STATISTIC(IndirectCalls, "Number of indirect calls");
STATISTIC(DirectCalls, "Number of direct calls");
STATISTIC(FunctionCount, "Number of functions");

STATISTIC(NumAdds, "Number of static additions for call graph");
STATISTIC(NumEdgePushes, "Number of static pushes for the call graph");
STATISTIC(NumStaticPushes, "Number of static pushes for the call graph");


using namespace llvm;
using namespace callgraph;


namespace callgraph {


// Insert an edge from the dummy node to the given function node. As always,
// we assume that the very first function node in the list is the dummy node.
// This has the effect of normalizing a degenerate call graph into acyclic
// subgraphs that are encodable within our graph numbering scheme.
CallEdge *
CallGraph::ensureDummyEdge(FunctionNode &fn) {
  CallNode &dummy = functions[0].calls.front();
  auto &edges = dummy.targets;

  for (auto &d :edges) {
    if (d.target == &fn) {
      return nullptr;
    }
  }

  edges.emplace_back(&functions[0], &fn);
  return &edges.back();
}


const int DEBUGGING = 0;


bool
CGBuilder::capturedInUser(User *user, Function *f, size_t start) {
  auto op = user->op_begin(), oe = user->op_end();
  size_t count = 0;
  while (op != oe && count < start) {
    ++op;
    ++count;
  }
  while (op != oe) {
    if (op->get() == f) {
      return true;
    }
    ++op;
  }
  return false;
}


std::vector<FunVector>
CGBuilder::createTargetMap(Module &m) {
  std::vector<FunVector> targetMap;
  for (auto &f : m) {
    // No need to consider a function if it never has its address taken.
    if (!f.hasAddressTaken()) {
      continue;
    }

    // TODO Handle mandatory minimum arguments? This may be imprecise
    size_t pos = f.isVarArg() ? 0 : f.getArgumentList().size() + 1;
    // TODO surely this is crazily inefficient
    while (targetMap.size() <= pos) {
      targetMap.push_back(std::vector<Function *>());
    }
    targetMap[pos].push_back(&f);
  }
  return targetMap;
}


int
CGBuilder::addCallees(FunctionNode &node, int nextID) {
  if (!node.fun) {
    return nextID;
  }

  Function &f = *node.fun;
  for (auto i = inst_begin(f), e = inst_end(f); i != e; ++i) {
    CallSite cs(&*i);
    if (cs.getInstruction()) {
      node.calls.emplace_back(cs, nextID);
      ++nextID;
    }
  }
  return nextID;
}



bool
CGBuilder::typesCompatible(Type *t1, Type *t2) {
  // If the values are of substantially different types or require
  // different amounts of memory, they are incompatible by the C standard.
  // We may not want to be this strict in practice, but it seems reasonable.
  if (t1->getTypeID() != t2->getTypeID()
      || td->getTypeStoreSize(t1) != td->getTypeStoreSize(t2)) {
    return false;
  }

  if (const StructType *s1 = dyn_cast<const StructType>(t1)) {
    const StructType *s2 = dyn_cast<const StructType>(t2);
    if (s1->getNumElements() != s2->getNumElements()
        || s1->isPacked() != s2->isPacked()) {
      return false;
    }
    for (unsigned i = 0, e = s2->getNumElements(); i != e; ++i) {
      if (!typesCompatible(s1->getElementType(i), s2->getElementType(i))) {
        return false;
      }
    }
  }

  return true;
}


// TODO We can be either conservative or strict here. The C standard, at least
// is much stricter than a lot of compilers seem to allow, and yields decent
// results. That is not unreasonable. A more conservative approach yields
// more explosive results. This could be pruned by alias analysis in the
// future.
bool
CGBuilder::canCallAs(const FunctionType *target,
                     const FunctionType *use,
                     CallSite cs,
                     size_t numArgs) {
  if ((numArgs > target->getNumParams() && !target->isVarArg())
      || (numArgs < target->getNumParams())
      || (cs.getInstruction()->hasNUsesOrMore(1)
        && !typesCompatible(target->getReturnType(), use->getReturnType()))
      || false) {
    return false;
  }

  auto parm = target->param_begin(), parme = target->param_end();
  auto arg = cs.arg_begin();
  while (parm != parme) {
    if (!typesCompatible(arg->get()->getType(), *parm)) {
      return false;
    }
    ++arg;
    ++parm;
  }

  return true;
}


void
CGBuilder::storeTargetsForCall(CallSite cs,
                               std::vector<FunVector> &targetMap,
                               FunVector &targets) {
  auto *ftyp = getFunType(cs.getCalledValue()->getType());
  size_t numArgs = cs.arg_size();
  if (numArgs + 1 < targetMap.size()) {
    for (auto f : targetMap[numArgs + 1]) {
      auto *ttyp = getFunType(f->getType());
      if (canCallAs(ttyp, ftyp, cs, numArgs)) {
        targets.push_back(f);
      }
    }
  }

  // TODO no code cloning
  for (auto f : targetMap[0]) {
    auto *ttyp = getFunType(f->getType());
    if (ttyp->getNumParams() <= numArgs
        && canCallAs(ttyp, ftyp, cs, numArgs)) {
      targets.push_back(f);
    }
  }
}


bool CGBuilder::runOnModule(Module &m) {
  DenseMap<Function*, FunctionNode*> funsToNodes;
  Function *entry = m.getFunction("main");
  assert(entry && "Entry point not found: main()");
  td = &getAnalysis<TargetData>();

  auto targetMap = createTargetMap(m);

  auto &callgraph = cg.functions;
  callgraph.push_back(FunctionNode(nullptr, callgraph.size()));
  for (auto &f : m) {
    callgraph.push_back(FunctionNode(&f, callgraph.size()));
    ++FunctionCount;
  }

  for (auto &f : callgraph) {
    funsToNodes[f.fun] = &f;
  }

  int callsiteID = 1;
  for (auto &f : callgraph) {
    callsiteID = addCallees(f, callsiteID);
    for (auto &c : f.calls) {
      if (c.isDirect()) {
        auto *targetNode = funsToNodes[util::getCallSiteTarget(c.cs)];
        c.targets.emplace_back(&f, targetNode);
        ++DirectCalls;
      } else {
        FunVector dests;
        ++IndirectCalls;
        storeTargetsForCall(c.cs, targetMap, dests);
        for (auto &d : dests) {
          c.targets.emplace_back(&f, funsToNodes[d]);
        }
      }
    }
  }

  FunctionNode &dummy = callgraph[0];
  dummy.calls.emplace_back(CallNode(CallSite(), 0));
  dummy.calls[0].targets.emplace_back(&dummy, funsToNodes[entry]);

  return false;
}

char CGBuilder::ID = 0;
RegisterPass<CGBuilder> X("buildcg", "Call Graph Building Pass");
const PassInfo *const CGBuilderID = &X;



struct DFSEntry {
  FunctionNode *node;
  std::vector<CallNode>::iterator call;
  std::list<CallEdge>::iterator to;
  DFSEntry (FunctionNode *node,
            std::vector<CallNode>::iterator call,
            std::list<CallEdge>::iterator target)
    : node(node),
      call(call),
      to(target) {
  }
};


void
CGEncodingPass::makeAcyclic() {
  std::vector<DFSEntry> dfsStack;
  std::vector<bool> seen(cg->functions.size(), false);
  std::vector<bool> inStack(cg->functions.size(), false);

  auto pushCall = [&dfsStack] (FunctionNode *fn) {
    auto callIt = fn->calls.begin();
    auto targetIt = fn->calls.end() == callIt
      ? std::list<CallEdge>::iterator()
      : callIt->targets.begin();
    dfsStack.emplace_back(fn, callIt, targetIt);
  };

  // Sadly, we can't use the DepthFirstIterator infrastructure here, because
  // we need to actually visit the backedges once in order to mark them and
  // insert the synthetic edges from the dummy node to the back edge target.
  // TODO: At least rip the DFS traversal into a back edge aware iterator to
  // clean up the orthogonal portions of this code itself.
  inStack[0] = seen[0] = true;
  pushCall(&cg->functions[0]);

  while (!dfsStack.empty()) {
    DFSEntry &pos = dfsStack.back();
    // First, if the target list we were scanning with pos is finished,
    // traverse back along the call graph to find the next target list
    // we need to scan.
    if (pos.call == pos.node->calls.end()) {
      inStack[dfsStack.back().node->id] = false;
      dfsStack.pop_back();
      continue;
    } else if (pos.to == pos.call->targets.end()){
      ++pos.call;
      if (pos.call != pos.node->calls.end()) {
        pos.to = pos.call->targets.begin();
      }
      continue;
    }

    // For each call edge in an active target list, we need to check for
    // back edges and fix them up.
    CallEdge &ce = *pos.to;
    FunctionNode *to = ce.target;
    ++pos.to;
    if (inStack[to->id]) {
      ce.isBackedge = true;
      ++BackEdges;
      cg->ensureDummyEdge(*to);
    } else if (!seen[to->id]) {
      seen[to->id] = true;
      inStack[to->id] = true;
      pushCall(to);
    }
  }
}


void
CGEncodingPass::addCallers() {
  for (auto &f : cg->functions) {
    for (auto &c : f.calls) {
      for (auto &d : c.targets) {
        auto &callers = callerMap[d.target];
        callers.push_back(&d);
      }
    }
  }
}


static std::vector<FunctionNode*>
topOrderedFunctions(CallGraph &cg) {
  std::vector<FunctionNode*> functions;
  std::copy(llvm::po_begin(&cg), llvm::po_end(&cg), 
            std::back_inserter(functions));
  std::reverse(functions.begin(), functions.end());
  return functions;
}


std::vector<CallEdge*>::iterator
partitionPushes(std::vector<CallEdge*> &callers) {
  return std::partition(callers.begin(), callers.end(),
    [](const CallEdge *edge) { return !edge->isBackedge; });
}


void
CGEncodingPass::computeIncomingIDs(FunctionNode *fn) {
  // Skip the synthetic entry node
  const FunctionNode *DUMMY = &cg->functions[0];
  if (DUMMY == fn) {
    numContexts[fn] = 1;
    return;
  }

  auto &callers = callerMap[fn];
  auto firstPush = partitionPushes(callers);
  CallEdge *addedDummy = nullptr;
  if (firstPush != callers.end()) {
    addedDummy = cg->ensureDummyEdge(*fn);
  }
  if (addedDummy) {
    size_t pos = firstPush - callers.begin();
    callers.insert(callers.begin(), addedDummy);
    firstPush = callers.begin() + pos + 1;
  }

  std::sort(callers.begin(), callers.end(),
    [=](CallEdge *edge1, CallEdge *edge2) -> bool {
    if (DUMMY == edge1->caller) {
      return true;
    } else if (DUMMY == edge2->caller) {
      return false;
    } else {
      // TODO ordering heuristic from profiles to avoid instrumenting hot calls
      return edge1 < edge2;
    }
  });

  // Compute the addition based encoding operations.
  // At this point, we already know that the IDs cannot overflow, so using
  // a bounded precision base should be safe.
  uint64_t idBase = 0;
  for (auto edge = callers.begin(); edge != firstPush; ++edge) {
    encodeOps[*edge] = EncodeOp{OpKind::ADD, idBase};
    idBase += numContexts[(*edge)->caller];
    ++NumAdds;
  }
  numContexts[fn] = idBase;

  // We need to uniquely identify the static site of all pushes, but
  // we won't have the site specific information until later. Init to 0.
  for (auto edge = firstPush, e = callers.end(); edge != e; ++edge) {
    encodeOps[*edge] = EncodeOp{OpKind::PUSH, 0};
    ++NumEdgePushes;
  }
}


void
CGEncodingPass::labelPushes() {
  uint64_t currentID = 1;
  for (auto &f : cg->functions) {
    for (auto &c : f.calls) {
      bool usesPush = false;
      for (auto &d : c.targets) {
        auto &op = encodeOps[&d];
        if (OpKind::PUSH == op.kind) {
          usesPush = true;
          op.val = currentID;
        }
      }

      // If we consumed the currentID, then we need to get a fresh one
      if (usesPush) {
        ++currentID;
        ++NumStaticPushes;
      }
    }
  }
}


bool
CGEncodingPass::runOnModule(Module &m) {
  cg = &getAnalysis<CGBuilder>().getCallGraph();
  makeAcyclic();
  addCallers();
  for (auto *fnNode : topOrderedFunctions(*cg)) {
    computeIncomingIDs(fnNode);
  }
  labelPushes();
  return false;
}


char CGEncodingPass::ID = 0;
RegisterPass<CGEncodingPass> CGE("encodecg", "Call Graph Encoding Pass");
const PassInfo *const CGEncodingPassID = &CGE;



void
CGEncodingInstrumentorPass::createGlobals(llvm::Module &m) {
  TargetData &td = getAnalysis<TargetData>();

  // We want to use the underlying word size for all operations.
  idTy = td.getIntPtrType(m.getContext());
  Constant *nullID = Constant::getNullValue(idTy);

  const size_t BUFFER_SIZE = 1024;
  auto *bufferTy = ArrayType::get(idTy, BUFFER_SIZE);

  id = new GlobalVariable(m, idTy, false,
                          GlobalValue::CommonLinkage,
                          nullID,
                          "PCCE.current_context_id");

  idStack = new GlobalVariable(m, bufferTy, false,
                               GlobalValue::CommonLinkage,
                               ConstantAggregateZero::get(bufferTy),
                               "PCCE.context_stack_buffer");

  Constant *indices[] = { nullID,  nullID };
  auto startPtr = ConstantExpr::getGetElementPtr(idStack,indices);
  stackStart = new GlobalVariable(m, idTy->getPointerTo(), false,
                                 GlobalValue::CommonLinkage,
                                 startPtr,
                                 "PCCE.context_stack_top");
}


void
CGEncodingInstrumentorPass::encodePossibleEdges(CallSite cs, 
                                                std::list<CallEdge> &edges) {
  std::vector<CallEdge*> adds;
  std::vector<CallEdge*> pushes;

  // filter out the adds and pushes
  // TODO sort adds based on edge weight / frequency / likelihood
  for (auto &edge : edges) {
    auto edgeOp = encoding->encodeOps[&edge];
    std::vector<CallEdge*> &opList =
      CGEncodingPass::OpKind::ADD == edgeOp.kind ? adds : pushes;
    opList.push_back(&edge);
  }

  // If there are only pushes, there is no need for splitting or guards
  if (adds.empty() && !pushes.empty()) {
    encodePush(cs, **pushes.begin());
    return;
  }

  // TODO handle exceptions
  auto &context = cs->getContext();
  auto precall = cs->getParent();
  auto postStart = ++BasicBlock::iterator(cs.getInstruction());
  auto postCall = precall->splitBasicBlock(postStart);
  Value *called = cs.getCalledValue();
  SmallVector<std::pair<Value*,BasicBlock*>,8> phiEdges;

  // Go through the list of inlined targets for addition. For each inlined
  // target, we split the current call block into a precall that conditionally
  // banches to the inlined call or to the degenerate case.
  for (auto &add : adds) {
    BasicBlock *oldCall = precall->splitBasicBlock(cs.getInstruction());

    // Create an inlined version of the call
    auto *inlined = BasicBlock::Create(context, "PCCE.inlined_call_block",
                                       oldCall->getParent(), oldCall);
    Value *target = add->target->fun;
    Type *calledType = called->getType();
    if (target->getType() != calledType) {
      target = new BitCastInst(target, calledType, "", inlined);
    }
    SmallVector<Value*,8> args;
    for (auto arg = cs.arg_begin(), e = cs.arg_end(); arg != e; ++arg) {
      args.push_back(*arg);
    }
    auto *call = CallInst::Create(target, args, "", inlined);
    BranchInst::Create(postCall, inlined);

    // Link the precall to the possible targets
    auto *oldTerm = precall->getTerminator();
    auto cond = new ICmpInst(oldTerm, ICmpInst::ICMP_EQ, called, target, "");
    auto *newTerm = BranchInst::Create(inlined, oldCall, cond);
    ReplaceInstWithInst(oldTerm, newTerm);

    // Reset the precall and store phi edges
    phiEdges.push_back(std::make_pair(call, inlined));
    precall = oldCall;
  }

  // Replace all uses of call with a phi node in postCall
  if (!cs->use_empty()) {
    PHINode *phi = PHINode::Create(cs.getType(), adds.size() + 1, "",
                                   postCall->begin());
    cs->replaceAllUsesWith(phi);
    for (auto phiEdge : phiEdges) {
      phi->addIncoming(phiEdge.first, phiEdge.second);
    }
    phi->addIncoming(cs.getInstruction(), cs->getParent());
  }

  // Finally, encode the push if there are any known uses.
  if (!pushes.empty()) {
    encodePush(cs, **pushes.begin());
  }
}


void
CGEncodingInstrumentorPass::encodeOneEdge(CallSite cs, CallEdge &edge) {
  auto &op = encoding->encodeOps[&edge];
  switch (op.kind) {
    case CGEncodingPass::OpKind::ADD:
      encodeAdd(cs, edge);
      break;

    case CGEncodingPass::OpKind::PUSH:
      // TODO Run length encoding of recursion
      encodePush(cs, edge);
      break;
  }
}


void
CGEncodingInstrumentorPass::encodeAdd(CallSite cs, CallEdge &edge) {
  auto &op = encoding->encodeOps[&edge];
  if (op.val) {
    // We only need any instrumentation when the value is nonzero.
    auto *before = cs.getInstruction();
    auto *value = ConstantInt::get(idTy, op.val);

    // Load and increment before the call.
    auto load = new LoadInst(id, "PCCE.preID", before);
    auto inc =
      BinaryOperator::Create(Instruction::Add, load, value, "", before);
    new StoreInst(inc, id, before);

    // Store the original value afterward. Doing it this was uses stack
    // space and kills the implicit encoding technique, but it's simpler
    // and makes the pattern for handling exceptions consistent.
    // TODO handle exceptions
    new StoreInst(load, id, ++BasicBlock::iterator(before));
  }
}


void
CGEncodingInstrumentorPass::encodePush(CallSite cs, CallEdge &edge) {
  auto *before = cs.getInstruction();
  Constant *one = ConstantInt::get(idTy, 1);
  Constant *two = ConstantInt::get(idTy, 2);
  uint64_t siteID = encoding->encodeOps[&edge].val;

  // Push the current ID and site before the call.
  auto loadID = new LoadInst(id, "PCCE.preID", before);
  auto stackPtr = new LoadInst(stackStart, "PCCE.stackPos", before);
  new StoreInst(loadID, stackPtr, before);
  auto sitePos = GetElementPtrInst::Create(stackPtr, one, "", before);
  new StoreInst(ConstantInt::get(idTy, siteID), sitePos, before);
  auto nextPos = GetElementPtrInst::Create(stackPtr, two, "", before);
  new StoreInst(nextPos, stackStart, before);
  new StoreInst(Constant::getNullValue(idTy), id, before);

  // Store the original values afterward. Doing it this was uses stack
  // space and kills the implicit encoding technique, but it's simpler
  // and makes the pattern for handling exceptions consistent.
  // TODO handle exceptions
  auto after = &*++BasicBlock::iterator(before);
  new StoreInst(stackPtr, stackStart, after);
  new StoreInst(loadID, id, after);
}


void
CGEncodingInstrumentorPass::encodeSite(CallNode &call) {
  assert(!call.isDummy() && "Encoding dummy call site");

  if (call.isDirect()) {
    // We don't need to guard the instrumentation
    assert(!call.targets.empty() && "Direct call to no targets!");
    encodeOneEdge(call.cs, *call.targets.begin());
  } else {
    // We need to guard different instrumentation types based on the target
    encodePossibleEdges(call.cs, call.targets); 
  }
}


bool
CGEncodingInstrumentorPass::runOnModule(Module &m) {
  cg = &getAnalysis<CGBuilder>().getCallGraph();
  encoding = &getAnalysis<CGEncodingPass>();

  createGlobals(m);

  for (auto &f : cg->functions) {
    if (!f.fun) {
      continue;
    }
    for (auto &c : f.calls) {
      encodeSite(c);
    }
  }

  return true;
}


char CGEncodingInstrumentorPass::ID = 0;
RegisterPass<CGEncodingInstrumentorPass>
  CGEI("instrumentcg","Call Graph Encoding Instrumentation Pass");
const PassInfo *const CGEncodingInstrumentorPassID = &CGEI;


}



