
#include <cstdlib>
#include <fstream>
#include <map>
#include <queue>
#include <sstream>

#include "./llvm_common.h"

#include "CodeXform.h"
#include "DFGAnalysis.h"
#include "DFGEntry.h"
#include "Pass.h"
#include "Transformation.h"
#include "Util.h"
#include "dsa-ext/rf.h"

using namespace llvm;

#define DEBUG_TYPE "stream-specialize"

bool StreamSpecialize::runOnModule(Module &M) {

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    ACT = &getAnalysis<AssumptionCacheTracker>(F).getAssumptionCache(F);
    LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
    DT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    AAR = &getAnalysis<AAResultsWrapperPass>(F).getAAResults();
    BFI = &getAnalysis<BlockFrequencyInfoWrapperPass>(F).getBFI();
    MSSA = &getAnalysis<MemorySSAWrapperPass>(F).getMSSA();
    runOnFunction(F);
  }

  // If we are not extracting DFG's, do not clean up the code with O3.
  if (!dsa::utils::ModuleContext().EXTRACT) {
    llvm::PassManagerBuilder PMB;
    PMB.OptLevel = 3;
    llvm::legacy::FunctionPassManager Fpass(&M);
    llvm::legacy::PassManager Mpass;
    PMB.populateFunctionPassManager(Fpass);
    PMB.populateModulePassManager(Mpass);
    PMB.Inliner = llvm::createFunctionInliningPass(3, 0, false);
    for (auto &F : M) {
      runOnFunction(F);
    }
    Fpass.doFinalization();
    Mpass.run(M);
  }

  return false;
}

bool StreamSpecialize::runOnFunction(Function &F) {

  if (!dsa::utils::ModuleContext().TEMPORAL) {
    dsa::xform::EliminateTemporal(F);
  }


  IRBuilder<> IB(F.getContext());
  IBPtr = &IB;

  auto ScopePairs = dsa::analysis::GatherConfigScope(F);
  if (!ScopePairs.empty()) {
    if (!dsa::utils::ModuleContext().EXTRACT) {
      DSARegs = dsa::xform::InjectDSARegisterFile(F);
    }
  } else {
    LLVM_DEBUG(DSA_INFO << "No need to transform " << F.getName() << "\n");
  }
  LLVM_DEBUG(DSA_INFO << "Transforming " << F.getName() << "\n");
  SCEVExpander SEE(*SE, F.getParent()->getDataLayout(), "");
  dsa::xform::CodeGenContext CGC(&IB, DSARegs, *SE, SEE);
  for (int I = 0, N = ScopePairs.size(); I < N; ++I) {
    std::string Name = F.getName().str() + "_dfg_" + std::to_string(I) + ".dfg";
    auto *Start = ScopePairs[I].first;
    auto *End = ScopePairs[I].second;
    DFGFile DF(Name, Start, End, this);
    dsa::analysis::ExtractDFGFromScope(DF, Start, End, DT, LI);
    auto SI = dsa::analysis::ExtractSpadFromScope(Start, End);

    std::vector<dsa::analysis::DFGLoopInfo> DLIs;
    for (auto *DFG : DF.DFGFilter<DFGBase>()) {
      DLIs.emplace_back(dsa::analysis::AnalyzeDFGLoops(DFG, *SE));
    }
    auto CMIs = dsa::analysis::GatherMemoryCoalescing(DF, *SE, DLIs);
    {
      std::error_code EC;
      llvm::raw_fd_ostream RFO(DF.getName(), EC);
      dsa::xform::EmitDFG(RFO, &DF, CMIs);
    }
    // If extraction only, we do not schedule and analyze.
    if (dsa::utils::ModuleContext().EXTRACT) {
      continue;
    }
    auto *SBCONFIG = getenv("SBCONFIG");
    CHECK(SBCONFIG);
    auto Cmd = formatv("ss_sched --dummy {0} {1} {2} -e 0 > /dev/null", "-v", SBCONFIG, Name).str();
    LLVM_DEBUG(DSA_INFO << Cmd);
    CHECK(system(Cmd.c_str()) == 0) << "Not successfully scheduled! Try another DFG!";
    // TODO(@were): When making the DFG's of index expression is done, uncomment
    // these. auto Graphs = DFG.DFGFilter<DFGBase>();
    // std::vector<std::vector<int>> Ports(Graphs.size());
    // for (int i = 0, n = Graphs.size(); i < n; ++i) {
    //   Ports[i].resize(Graphs[i]->Entries.size(), -1);
    // }
    // DF.InspectSPADs();
    auto CI = dsa::analysis::ExtractDFGPorts(Name, DF, CMIs);
    dsa::xform::InjectConfiguration(CGC, CI, Start, End);
    if (dsa::utils::ModuleContext().EXTRACT) {
      continue;
    }
    dsa::xform::InjectStreamIntrinsics(CGC, DF, CMIs, SI);
    DF.EraseOffloadedInstructions();
  }
  if (!dsa::utils::ModuleContext().EXTRACT) {
    for (auto &Pair : ScopePairs) {
      Pair.second->eraseFromParent();
      Pair.first->eraseFromParent();
    }
  } else {
    return false;
  }

  LLVM_DEBUG(llvm::errs() << "After transformation:\n"; F.dump());

  return false;
}

void StreamSpecialize::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AssumptionCacheTracker>();
  AU.addRequired<BlockFrequencyInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<DependenceAnalysisWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetTransformInfoWrapperPass>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<LoopAccessLegacyAnalysis>();
  AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<MemorySSAWrapperPass>();
}

char StreamSpecialize::ID = 0;

static RegisterPass<StreamSpecialize>
    X("stream-specialize", "Decoupld-Spatial Specialized Transformation");
