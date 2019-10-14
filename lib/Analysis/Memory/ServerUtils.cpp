//===--- ServerUtils.cpp ---- Analysis Server Utils -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===----------------------------------------------------------------------===//
//
// This file implements functions to simplify client to server data mapping.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/Utils.h"
#include <llvm/IR/InstIterator.h>
#include "llvm/IR/InstVisitor.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>

using namespace llvm;
using namespace tsar;

void ClientToServerMemory::prepareToClone(
    llvm::Module &ClientM, llvm::ValueToValueMapTy &ClientToServer) {
  // By default global metadata variables and some of local variables are not
  // cloned. This leads to implicit references to the original module.
  // For example, traverse of MetadataAsValue for the mentioned variables
  // visits DbgInfo intrinsics in both modules (clone and origin).
  // So, we perform preliminary manual cloning of local variables.
  for (auto &F : ClientM) {
    for (auto &I : instructions(F))
      if (auto DDI = dyn_cast<DbgInfoIntrinsic>(&I)) {
        MapMetadata(cast<MDNode>(DDI->getVariable()), ClientToServer);
        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        DDI->getAllMetadata(MDs);
        for (auto MD : MDs)
          MapMetadata(MD.second, ClientToServer);
      }
    // Cloning of a function do not duplicate DISubprogram metadata, however
    // cloning of variables implemented above already duplicate this metadata.
    // So, we revert this cloning to prevent changes of behavior of other
    // cloning methods. For example, cloned metadata-level memory locations
    // must point to the original function because after rebuilding alias tree
    // metadata attached to a function will be used and this metadata is always
    // points to the original DISubprogram.
    if (auto *MD = findMetadata(&F))
      (*ClientToServer.getMDMap())[MD].reset(MD);
  }
}

namespace {
struct DICompileUnitSearch {
  void visitMDNode(MDNode &MD) {
    if (!MDNodes.insert(&MD).second)
      return;
    for (Metadata *Op : MD.operands()) {
      if (!Op)
        continue;
      if (auto *CU = dyn_cast<DICompileUnit>(Op))
        CUs.push_back(CU);
      if (auto *N = dyn_cast<MDNode>(Op)) {
        visitMDNode(*N);
        continue;
      }
    }
  }

  SmallPtrSet<const Metadata *, 32> MDNodes;
  SmallVector<DICompileUnit *, 2> CUs;
};
}

void ClientToServerMemory::initializeServer(
    llvm::Pass &P, llvm::Module &ClientM, llvm::Module &ServerM,
    llvm::ValueToValueMapTy &ClientToServer, llvm::legacy::PassManager &PM) {
  // Add list DICompileUnits (this may occur due to manual mapping performed
  // in prepareToClone().
  // TODO (kaniandr@gmail.com): it seems that the appropriate check will be
  // added in the next LLVM release (in 10 version). So, remove this check.
  auto *ServerCUs = ServerM.getOrInsertNamedMetadata("llvm.dbg.cu");
  SmallPtrSet<const void *, 8> Visited;
  for (const auto *CU : ServerCUs->operands())
    Visited.insert(CU);
  DICompileUnitSearch Search;
  for (auto &F : ServerM) {
    SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
    F.getAllMetadata(MDs);
    for (auto &MD : MDs)
      Search.visitMDNode(*MD.second);
  }
  for (auto *CU : Search.CUs)
    if (Visited.insert(CU).second)
      ServerCUs->addOperand(CU);
  // Prepare to mapping from origin to cloned DIMemory.
  auto &Env = P.getAnalysis<DIMemoryEnvironmentWrapper>();
  MDToDIMemoryMap CloneToOrigin;
  for (auto &F : ClientM) {
    auto DIAT = Env->get(F);
    if (!DIAT)
      continue;
    for (auto &DIM : make_range(DIAT->memory_begin(), DIAT->memory_end())) {
      auto MD = ClientToServer.getMappedMD(DIM.getAsMDNode());
      assert(MD && "Mapped metadata for a specified memory must exist!");
      CloneToOrigin.try_emplace(cast<MDNode>(*MD), &DIM);
    }
  }
  // Passes are removed in backward direction and handlers should be removed
  // before memory environment.
  PM.add(createClonedDIMemoryMatcherStorage());
  PM.add(createDIMemoryEnvironmentStorage());
  PM.add(createClonedDIMemoryMatcher(std::move(CloneToOrigin)));
}

void ClientToServerMemory::getAnalysisUsage(AnalysisUsage& AU) {
  AU.addRequired<DIMemoryEnvironmentWrapper>();
}
