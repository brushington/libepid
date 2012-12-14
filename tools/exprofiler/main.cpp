#include "llvm/ADT/OwningPtr.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Assembly/Parser.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/IRReader.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Constants.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Linker.h"
#include "llvm/PassManager.h"
#include "llvm/Pass.h"

#include <algorithm>

#include "libepid/CallGraph.h"
#include "libepid/SoftInstCounter.h"
#include "libepid/StructuralIndexing.h"

using namespace std;
using namespace llvm;
using namespace callgraph;
using namespace libepid;


//////////////////////////////////////////////////////////////////////////////
//  Options
//////////////////////////////////////////////////////////////////////////////


namespace {

  cl::opt<std::string>
  inMFile(cl::Positional,
          cl::desc("<Module to transform>"),
          cl::value_desc("filename"), cl::Required);

  cl::opt<std::string>
  outFile("o",
          cl::desc("Output module to create"),
          cl::value_desc("filename"),
          cl::Required);

}


//////////////////////////////////////////////////////////////////////////////
//  Basic module utilities
//////////////////////////////////////////////////////////////////////////////


void
saveModule(Module &m, std::string filename) {
  std::string errorMsg;
  raw_fd_ostream out(filename.c_str(), errorMsg, raw_fd_ostream::F_Binary);

  if (!errorMsg.empty()){
    report_fatal_error("error saving llvm module to '" + filename + "': \n"
                       + errorMsg);
  }
  WriteBitcodeToFile(&m, out);
}


void
instrumentModule(Module &m) {
  // TODO
  CGBuilder *builder = new CGBuilder();
  CGEncodingPass *encoder = new CGEncodingPass();

//   PassManager pm;
//   pm.add(new TargetData(&m));
//   pm.add(builder);
//   pm.add(encoder);
//   pm.add(new CGEncodingInstrumentorPass());
//   pm.run(m);
  PassManager pm;
  pm.add(new TargetData(&m));
  pm.add(createUnifyFunctionExitNodesPass());
  pm.add(new LoopInfo());
  pm.add(new PostDominatorTree());
  pm.add(new StructuralIndexingPass());
  pm.run(m);

  WriteGraph(outs(), &builder->getCallGraph());
  outs() << "Done\n";
}


//////////////////////////////////////////////////////////////////////////////
//  Entry Point
//////////////////////////////////////////////////////////////////////////////


int
main (int argc, char ** argv) {
  sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj shutdown;  // Call llvm_shutdown() on exit.
  LLVMContext &context = getGlobalContext();
  cl::ParseCommandLineOptions(argc, argv);

  sys::Path outputPath(outFile);
  if (inMFile.getValue() == outputPath.str()) {
    report_fatal_error("Input and output modules are the same!\n"
                       "Bailing out to avoid overwriting.");
  }

  SMDiagnostic err;
  OwningPtr<Module> module;
  module.reset(ParseIRFile(inMFile.getValue(), err, context));

  if (!module.get()) {
    errs() << "Error reading bitcode file.\n";
    err.print(argv[0], errs());
    return -1;
  }

  instrumentModule(*module);
  saveModule(*module, outputPath.str());

  outs() << "Done creating:\n" << outputPath.str() << "\n";

  return 0;
}

