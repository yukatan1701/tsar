//===---- tsar_dbg_output.cpp - Output functions for debugging --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements a set of output functions.
//
//===----------------------------------------------------------------------===//

#include "tsar_dbg_output.h"
#include "DIEstimateMemory.h"
#include "DIMemoryLocation.h"
#include "DIUnparser.h"
#include "tsar_pass.h"
#include "SourceUnparserUtils.h"
#include "tsar_utility.h"
#include <utility.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace tsar {
void printLocationSource(llvm::raw_ostream &O, const Value *Loc,
    const DominatorTree *DT) {
  if (!Loc)
    O << "?";
  else if (!unparsePrint(O, Loc, DT))
    Loc->printAsOperand(O, false);
}

void printLocationSource(llvm::raw_ostream &O, const llvm::MemoryLocation &Loc,
    const DominatorTree *DT) {
  O << "<";
  printLocationSource(O, Loc.Ptr, DT);
  O << ", ";
  if (Loc.Size == MemoryLocation::UnknownSize)
    O << "?";
  else
    O << Loc.Size;
  O << ">";
}

void printDILocationSource(unsigned DWLang,
    const DIMemoryLocation &Loc, raw_ostream &O) {
  if (!Loc.isValid()) {
    O << "<";
    O << "invalid";
    if (Loc.Var)
      O << "(" << Loc.Var->getName() << ")";
    O << ",?>";
    return;
  }
  O << "<";
  if (!unparsePrint(DWLang, Loc, O))
    O << "?" << Loc.Var->getName() << "?";
  O << ", ";
  auto Size = Loc.getSize();
  if (Size == MemoryLocation::UnknownSize)
    O << "?";
  else
    O << Size;
  O << ">";
}

void printDILocationSource(unsigned DWLang,
    const DIMemory &Loc, llvm::raw_ostream &O) {
  auto M = const_cast<DIMemory *>(&Loc);
  if (auto EM = dyn_cast<DIEstimateMemory>(M)) {
    printDILocationSource(DWLang,
      { EM->getVariable(), EM->getExpression(), EM->isTemplate() }, O);
  } else if (auto UM = dyn_cast<DIUnknownMemory>(M)) {
    auto DbgLoc = UM->getDebugLoc();
    auto MD = UM->getMetadata();
    assert(MD && "MDNode must not be null!");
    if (UM->isCall())
      if (!isa<DISubprogram>(MD))
        O << "?()";
      else
        O << cast<DISubprogram>(MD)->getName() << "()";
    else
      if (!isa<DISubprogram>(MD))
        O << "<?,?>";
      else
        O << "<" << cast<DISubprogram>(MD)->getName() << ",?>";
    if (DbgLoc)
      O << ":" << DbgLoc.getLine() << ":" << DbgLoc.getCol();
  } else {
      O << "<?, ?>";
  }
}

void printDIType(raw_ostream &o, const DITypeRef &DITy) {
  Metadata *DITyVal = DITy;
  bool isDerived = false;
  if (auto *DITy = dyn_cast_or_null<DIDerivedType>(DITyVal)) {
    DITyVal = DITy->getBaseType();
    isDerived = true;
  }
  if (DIType *DITy = dyn_cast_or_null<DIType>(DITyVal))
    o << DITy->getName();
  else
    o << "<unknown type>";
  if (isDerived)
    o << "*";
}

void printDIVariable(raw_ostream &o, DIVariable *DIVar) {
  assert(DIVar && "Variable must not be null!");
  o << DIVar->getLine() << ": ";
  printDIType(o, DIVar->getType()), o << " ";
  o << DIVar->getName();
}

namespace {
void printLoops(llvm::raw_ostream &o, const Twine &Offset,
                LoopInfo::reverse_iterator ReverseI,
                LoopInfo::reverse_iterator ReverseEI) {
  for (; ReverseI != ReverseEI; --ReverseEI) {
    (Offset + "- ").print(o);
    DebugLoc loc = (*ReverseI)->getStartLoc();
    loc.print(o);
    o << "\n";
    printLoops(o, Offset + "\t", (*ReverseI)->rbegin(), (*ReverseI)->rend());
  }
}
}

void printLoops(llvm::raw_ostream &o, const LoopInfo &LI) {
  printLoops(o, "", LI.rbegin(), LI.rend());
}
}

namespace {
/// \brief Prints analysis info for function passes.
///
/// This class is similar to printers which is used in the opt tool.
class FunctionPassPrinter : public FunctionPass, public bcl::Uncopyable {
public:
  static char ID;

  FunctionPassPrinter(const PassInfo *PI, raw_ostream &Out)
    : FunctionPass(ID), mPassToPrint(PI), mOut(Out) {
    assert(PI && "PassInfo must not be null!");
    std::string PassToPrintName = mPassToPrint->getPassName();
    mPassName = "FunctionPass Printer: " + PassToPrintName;
  }

  bool runOnFunction(Function &F) override {
    mOut << "Printing analysis '" << mPassToPrint->getPassName()
      << "' for function '" << F.getName() << "':\n";
    getAnalysisID<Pass>(mPassToPrint->getTypeInfo()).
      print(mOut, F.getParent());
    return false;
  }

  StringRef getPassName() const override { return mPassName; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequiredID(mPassToPrint->getTypeInfo());
    AU.setPreservesAll();
  }

private:
  const PassInfo *mPassToPrint;
  raw_ostream &mOut;
  std::string mPassName;
};

char FunctionPassPrinter::ID = 0;
}

/// \brief Creates a pass to print analysis info for function passes.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
FunctionPass *llvm::createFunctionPassPrinter(
    const PassInfo *PI, raw_ostream &OS) {
  return new FunctionPassPrinter(PI, OS);
}