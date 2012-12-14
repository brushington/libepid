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


#ifndef LIBEPID_CALLGRAPH_H
#define LIBEPID_CALLGRAPH_H

#include <vector>
#include <list>
#include <iterator>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Target/TargetData.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Pass.h"

#include "util/CallSites.h"

using namespace llvm;

namespace callgraph {

struct FunctionNode;
struct CallNode;
struct CallEdge;

struct CallEdge {
  FunctionNode *target;
  FunctionNode *caller;
  bool isBackedge;

  CallEdge(FunctionNode *caller, FunctionNode *target)
    : target(target),
      caller(caller),
      isBackedge(false) {
  }
};

struct FunctionNode {
  Function *fun;
  int id;
  std::vector<CallNode> calls;

  FunctionNode(Function *fun, int id)
    : fun(fun),
      id(id) {
  }

  llvm::StringRef getName() const {
    return fun ? fun->getName() : "<<Dummy Node>>";
  }
};

struct CallNode {
  CallSite cs;
  int id;
  // We need to keep track of positions in these lists as well as pointers
  // to call edges while still performing updates like dummy node call
  // insertion. We can either use linked lists or a layer of indirection for
  // each pointer. For now, we just use lists, but we could benchmark both.
  std::list<CallEdge> targets;

  CallNode(CallSite cs, int id)
    : cs(cs),
      id(id) {
  }

  bool isDirect() {
    return !isDummy() && util::getCallSiteTarget(cs);
  }

  bool isDummy() {
    return !cs.getInstruction();
  }
};


struct CallGraph {
  std::vector<FunctionNode> functions;

  CallEdge * ensureDummyEdge(FunctionNode &fn);

};


typedef std::vector<Function *> FunVector;

struct CGBuilder : public ModulePass {
  static char ID;

  CGBuilder() : ModulePass(ID), cg(), td(nullptr) {}

private:
  CallGraph cg;

  llvm::TargetData *td;

  bool capturedInUser(User *user, Function *f, size_t start);

  std::vector<FunVector> createTargetMap(Module &m);

  int addCallees(FunctionNode &node, int nextID);

  const FunctionType* getFunType(const Type *funPtrTyp) {
    const PointerType *ptyp = cast<const PointerType>(funPtrTyp);
    return cast<FunctionType>(ptyp->getElementType());
  }

  bool typesCompatible(Type *t1, Type *t2);

  bool canCallAs(const FunctionType *target,
                 const FunctionType *use,
                 CallSite cs,
                 size_t numArgs);

  void storeTargetsForCall(CallSite cs,
                           std::vector<FunVector> &targetMap,
                           FunVector &targets);

public:
  CallGraph & getCallGraph() {
    return cg;
  }

  virtual bool runOnModule(Module &m);

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<llvm::TargetData>();
//TODO     AU.addRequired<llvm::AliasAnalysis>();
//     AU.addPreserved<llvm::AliasAnalysis>();
  }
};

extern RegisterPass<CGBuilder> X;
extern const PassInfo *const CGBuilderID;


struct CGEncodingPass : public ModulePass {

  enum class OpKind : unsigned char { ADD, PUSH };
  struct EncodeOp {
    OpKind kind;
    uint64_t val;
  };

  static char ID;
  CallGraph *cg;
  llvm::DenseMap<FunctionNode*,std::vector<CallEdge*>> callerMap;
  llvm::DenseMap<FunctionNode*,uint64_t> numContexts;
  llvm::DenseMap<CallEdge*,EncodeOp> encodeOps;
  llvm::DenseMap<CallSite*,uint64_t> pushIDs;

  CGEncodingPass() : ModulePass(ID), cg(nullptr) {}

public:

  virtual bool runOnModule(Module &m);

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<CGBuilder>();
    AU.addPreserved<llvm::AliasAnalysis>();
  }

  void makeAcyclic();

  void addCallers();

  void computeIncomingIDs(FunctionNode *fn);

  void labelPushes();
};

extern RegisterPass<CGEncodingPass> CGE;
extern const PassInfo *const CGEncodingPassID;


struct CGEncodingInstrumentorPass : public ModulePass {
  static char ID;
private:
  CallGraph *cg;
  CGEncodingPass *encoding;

  llvm::Type *idTy;
  llvm::Constant *id;
  llvm::Constant *idStack;
  llvm::Constant *stackStart;

  void createGlobals(llvm::Module &m);

  void encodeSite(CallNode &call);

  void encodePossibleEdges(CallSite cs, std::list<CallEdge> &edges);

  void encodeOneEdge(CallSite cs, CallEdge &edge);

  void encodeAdd(CallSite cs, CallEdge &edge);

  void encodePush(CallSite cs, CallEdge &edge);

public:

  CGEncodingInstrumentorPass() : ModulePass(ID), cg(nullptr) {}

  virtual bool runOnModule(Module &m);

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequired<llvm::TargetData>();
    AU.addRequired<CGBuilder>();
    AU.addRequired<CGEncodingPass>();
  }
};


}


namespace llvm {

template <> struct GraphTraits<callgraph::CallGraph*> {
  typedef callgraph::FunctionNode NodeType;
  struct ChildIteratorType
      : public std::iterator<std::input_iterator_tag, int> {
    std::vector<callgraph::CallNode>::const_iterator siteIt;
    std::vector<callgraph::CallNode>::const_iterator siteEnd;
    std::list<callgraph::CallEdge>::const_iterator targetIt;
    ChildIteratorType(std::vector<callgraph::CallNode>::const_iterator it,
                      std::vector<callgraph::CallNode>::const_iterator end)
      : siteIt(it),
        siteEnd(end),
        targetIt() {
      if (siteIt != siteEnd) {
        targetIt = siteIt->targets.begin();
      }
    }
    ChildIteratorType(const ChildIteratorType &other)
      : siteIt(other.siteIt),
        siteEnd(other.siteEnd),
        targetIt(other.targetIt)
        { }

    ChildIteratorType& operator++() {
      ++targetIt;
      if (targetIt == siteIt->targets.end()) {
        ++siteIt;
        if (siteIt != siteEnd) {
          targetIt = siteIt->targets.begin();
        }
      }
      return *this;
    }
    ChildIteratorType operator++(int) {
      ChildIteratorType result(*this);
      ++*this;
      return result;
    }
    bool operator==(const ChildIteratorType& rhs) const {
      if (siteIt != rhs.siteIt) {
        return false;
      }
      return siteIt == siteEnd || targetIt == rhs.targetIt;
    }
    bool operator!=(const ChildIteratorType& rhs) const {
      return !(*this == rhs);
    }
    callgraph::FunctionNode* operator*() const {
      return targetIt->target;
    }
    const callgraph::CallEdge & asEdge() const {
      return *targetIt;
    }
  };


  static NodeType *getEntryNode(callgraph::CallGraph *CG) {
    return &CG->functions[0];
  }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return ChildIteratorType{ N->calls.begin(), N->calls.end() };
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return ChildIteratorType{  N->calls.end(), N->calls.end() };
  }

  typedef std::vector<callgraph::FunctionNode>::iterator nodes_iterator;
  static nodes_iterator nodes_begin(callgraph::CallGraph *CG) {
    return CG->functions.begin();
  }
  static nodes_iterator nodes_end  (callgraph::CallGraph *CG){
    return CG->functions.end();
  }
  static unsigned size (const callgraph::CallGraph *CG) {
    return CG->functions.size();
  }
};


template<>
struct DOTGraphTraits<callgraph::CallGraph*>
  : public DefaultDOTGraphTraits {

  DOTGraphTraits (bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(const callgraph::CallGraph *CG) {
    std::string name("Approximate call graph");
    if (CG->functions.size() > 1) {
      Module *m = CG->functions[1].fun->getParent();
      name += " for " + m->getModuleIdentifier();
    }
    return name;
  }

  static std::string getSimpleNodeLabel(const callgraph::FunctionNode *Node,
                                        const callgraph::CallGraph *) {
    return Node->getName();
  }

  static std::string getCompleteNodeLabel(const callgraph::FunctionNode *Node, 
                                          const callgraph::CallGraph *CG) {
    std::string Str(getSimpleNodeLabel(Node, CG));
    raw_string_ostream OS(Str);

    OS << "\n";
    if (Node->fun) {
      Node->fun->getFunctionType()->print(OS);
      OS << "\n" << (!Node->fun->isVarArg() ? "non ":"") << "vararg\n";
    }

    return OS.str();
  }

  std::string getNodeLabel(const callgraph::FunctionNode *Node,
                           const callgraph::CallGraph *Graph) {
    if (isSimple())
      return getSimpleNodeLabel(Node, Graph);
    else
      return getCompleteNodeLabel(Node, Graph);
  }

  typedef GraphTraits<callgraph::CallGraph*>::ChildIteratorType EdgeIter;
  static std::string getEdgeAttributes(const void *, EdgeIter it,
                                       const callgraph::CallGraph*) {
    if (it.asEdge().isBackedge) {
      return "style=dotted";
    } else {
      return "style=solid";
    }
  }
};

}


#endif

