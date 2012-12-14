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


#ifndef LIBEPID_SOFTINSTCOUNTER_H
#define LIBEPID_SOFTINSTCOUNTER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/Pass.h"

#include "libepid/ExecutionPoint.h"


class llvm::Type;
class llvm::Constant;


namespace libepid {


struct SoftInstCounterPass
    : public llvm::ModulePass, public ExecutionPointProvider {

  static char ID;

private:
  size_t pointCount;

  llvm::IntegerType *counterTy;
  llvm::Constant *counter;
  llvm::Constant *pointBuffer;

  llvm::DenseMap<llvm::Instruction*, size_t> pointIDs;

  void incrementCounter(llvm::Instruction *pos);

public:

  SoftInstCounterPass() : ModulePass(ID), pointCount(0) {}

  virtual bool runOnModule(llvm::Module &m);

  virtual ExecutionPoint createCurrentPoint(llvm::Instruction *point);
  
};


}


#endif

