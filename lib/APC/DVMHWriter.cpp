//===--- DVMHWriter.cpp ---- DVMH Program Generator -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
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
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to generate a DVMH program according to
// parallel variant obtained on previous steps of parallelization.
//
//===----------------------------------------------------------------------===//

#include "AstWrapperImpl.h"
#include "DistributionUtils.h"
#include "tsar/APC/Passes.h"
#include "tsar/APC/APCContext.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "ASTImportInfo.h"
#include "ClangUtils.h"
#include "Diagnostic.h"
#include "GlobalInfoExtractor.h"
#include "tsar_memory_matcher.h"
#include "tsar_pass_provider.h"
#include "tsar_pragma.h"
#include "tsar_transformation.h"
#include <apc/Distribution/DvmhDirective.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <bcl/utility.h>
#include <clang/AST/Decl.h>
#include <clang/AST/ASTContext.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Pass.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-dvmh-writer"

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
/// Collect declaration traits.
class DeclarationInfoExtractor :
  public RecursiveASTVisitor<DeclarationInfoExtractor> {
public:
  /// Description of a declaration.
  struct DeclarationInfo {
    /// If set to `false` then a declaration statement for an appropriate
    /// declaration contains multiple declarations (for example, `int X, Y`).
    bool IsSingleDeclStmt = false;
  };

  /// Map from declaration to its traits.
  using DeclarationInfoMap = DenseMap<unsigned, DeclarationInfo>;

  explicit DeclarationInfoExtractor(DeclarationInfoMap &Decls) :
    mDecls(Decls) {}

  bool VisitDeclStmt(DeclStmt *DS) {
    for (auto *D : DS->decls())
      if (isa<VarDecl>(D)) {
        auto Loc = D->getLocation();
        mDecls[Loc.getRawEncoding()].IsSingleDeclStmt = DS->isSingleDecl();
      }
    return true;
  }

private:
  DeclarationInfoMap &mDecls;
};

class APCDVMHWriter : public ModulePass, private bcl::Uncopyable {
  /// Description of a template which is necessary for source-to-source
  /// transformation.
  struct TemplateInfo {
    /// If set to `false` then no definitions of a template exists in a source
    /// code. Note, that declarations with `extern` specification may exist.
    bool HasDefinition = false;
  };

  /// Contains templates which are used in program files.
  using TemplateInFileUsage =
    DenseMap<FileID, SmallDenseMap<apc::Array *, TemplateInfo, 1>>;

  /// Set of variable declarations.
  using DeclarationSet = DenseSet<VarDecl *>;

  using DeclarationInfo = DeclarationInfoExtractor::DeclarationInfo;
  using DeclarationInfoMap = DeclarationInfoExtractor::DeclarationInfoMap;

public:
  static char ID;

  APCDVMHWriter() : ModulePass(ID) {
    initializeAPCDVMHWriterPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override {
    mCtx = nullptr;
    mRewriter = nullptr;
    mTransformedFiles.clear();
    mInsertedDirs.clear();
  }

private:
  /// Insert distribution directives for templates into source files.
  ///
  /// This add `#pragma dvm template [...]...[...] distribute [...]...[...]`
  /// directive and declarations (and one definition) for each template:
  /// `[extern] void *Name;`. If template does not used in a file the mentioned
  /// constructs are not inserted in this file.
  /// Definition will be inserted in source file (not include file) only.
  /// \post
  /// - If definition of template has been created then `HasDefinition` flag
  /// is set to true for this template.
  void insertDistibution(const apc::ParallelRegion &Region,
    const apc::DataDirective &DataDirx, TransformationContext &TfmCtx,
    TemplateInFileUsage &Templates);

  /// Insert `align` and `array` directives according to a specified align rule
  /// for all redeclarations of a specified variable. Emit diagnostics in case
  /// of errors.
  ///
  // TODO (kaniandr@gmail.com): split declaration statement if it contains
  // multiple declarations.
  // TODO (kaniandr@gmail.com): insert new definition if it is not found,
  // for example we do not treat definitions in include files as definitions
  // and do not insert align directives before such definitions.
  SourceLocation insertAlignment(const ASTImportInfo &Import,
    const DeclarationInfoMap &Decls, const apc::AlignRule &AR,
    const VarDecl *VD, TransformationContext &TrmCtx);

  /// Initialize declaration information for global declarations and
  /// collect all canonical declarations (including the local ones).
  ///
  /// \post
  /// - Set `IsSingleDeclStmt` property for global declarations only.
  /// If `Decls` does not contain a declaration then this container will be
  /// updated and declaration will be inserted.
  /// - Canonical declarations for all declarations will be stored in
  /// `CanonicalDecls` container.
  void initializeDeclInfo(const TranslationUnitDecl &Unit,
      const ASTImportInfo &ImportInfo, DeclarationInfoMap &Decls,
      DeclarationSet &CanonicalDecls) {
    DenseMap<unsigned, SourceLocation> FirstGlobalAtLoc;
    auto checkSingleDecl = [&FirstGlobalAtLoc, &Decls](
        SourceLocation StartLoc, SourceLocation Loc) {
      auto Info =
        FirstGlobalAtLoc.try_emplace(StartLoc.getRawEncoding(), Loc);
      if (!Info.second) {
        Decls[Info.first->second.getRawEncoding()].IsSingleDeclStmt = false;
        Decls[Loc.getRawEncoding()].IsSingleDeclStmt = false;
      } else {
        Decls[Loc.getRawEncoding()].IsSingleDeclStmt = true;
      }
    };
    for (auto *D : Unit.decls()) {
      if (auto *FD = dyn_cast<FunctionDecl>(D)) {
        if (FD->hasBody())
          for (auto *D : FD->decls())
            if (isa<VarDecl>(D)) {
              CanonicalDecls.insert(cast<VarDecl>(D->getCanonicalDecl()));
              Decls[D->getLocation().getRawEncoding()].IsSingleDeclStmt = false;
            }
      } else if (auto *VD = dyn_cast<VarDecl>(D)) {
        CanonicalDecls.insert(cast<VarDecl>(VD->getCanonicalDecl()));
        auto &MergedLocItr = ImportInfo.RedeclLocs.find(VD);
        if (MergedLocItr == ImportInfo.RedeclLocs.end()) {
          checkSingleDecl(VD->getLocStart(), VD->getLocation());
        } else {
          auto &StartLocs = MergedLocItr->second.find(VD->getLocStart());
          auto &Locs = MergedLocItr->second.find(VD->getLocation());
          for (std::size_t I = 0, EI = Locs.size(); I < EI; ++I)
            checkSingleDecl(StartLocs[I], Locs[I]);
        }
      }
    }
  }

  /// Insert a specified data directive `DirStr` in a specified location `Where`
  /// or diagnose error if insertion is not possible.
  void insertDataDirective(SourceLocation DeclLoc,
      const DeclarationInfoMap &Decls, SourceLocation Where, StringRef DirStr) {
    assert(Where.isValid() && "Location must be valid!");
    assert(DeclLoc.isValid() && "Location must be valid!");
    auto &Diags = mCtx->getDiagnostics();
    if (Where.isMacroID()) {
      toDiag(Diags, Where, diag::err_apc_insert_dvm_directive) << DirStr.trim();
      toDiag(Diags, DeclLoc, diag::note_apc_insert_macro_prevent);
      return;
    }
    auto DInfoItr = Decls.find(DeclLoc.getRawEncoding());
    // DeclarationInfo is not available for functions.
    if (DInfoItr == Decls.end() || DInfoItr->second.IsSingleDeclStmt) {
      insertDirective(Where, DirStr);
    } else {
      toDiag(Diags, Where, diag::err_apc_insert_dvm_directive) << DirStr.trim();
      toDiag(Diags, DeclLoc, diag::note_apc_not_single_decl_stmt);
    }
  }

  /// Insert a specified directive in a specified location or diagnose error
  /// if other directive has been already inserted at the same point.
  void insertDirective(SourceLocation Where, StringRef DirStr) {
    assert(Where.isValid() && "Location must be valid!");
    assert(Where.isFileID() && "Location must not be in macro!");
    Where = getLocationToTransform(Where);
    assert(Where.isValid() && "Location must be valid!");
    auto Itr = mInsertedDirs.find(Where.getRawEncoding());
    if (Itr != mInsertedDirs.end()) {
      if (Itr->second == DirStr)
        return;
      auto &Diags = mCtx->getDiagnostics();
      toDiag(Diags, Where, diag::err_apc_insert_dvm_directive) << DirStr.trim();
      toDiag(Diags, Where, diag::note_apc_insert_multiple_directives);
      return;
    }
    mRewriter->InsertTextBefore(Where, DirStr);
    auto &SrcMgr = mCtx->getSourceManager();
    if (Where != getStartOfLine(Where, SrcMgr))
      mRewriter->InsertTextBefore(Where, "\n");
    mTransformedFiles.try_emplace(
      SrcMgr.getFilename(Where), SrcMgr.getFileID(Where));
    mInsertedDirs.try_emplace(Where.getRawEncoding(), DirStr);
  }

  /// If file which contains a specified location `Loc` has been already
  /// transformed return location which points to the same point as `Loc` in
  /// a transformed file.
  SourceLocation getLocationToTransform(SourceLocation Loc) {
    assert(Loc.isValid() && "Location must be valid!");
    assert(Loc.isFileID() && "Location must not be in macro!");
    auto &SrcMgr = mCtx->getSourceManager();
    auto Filename = SrcMgr.getFilename(Loc);
    assert(!Filename.empty() && "File must be known for a specified location!");
    auto FileItr = mTransformedFiles.find(Filename);
    if (FileItr == mTransformedFiles.end())
      return Loc;
    auto DecLoc = SrcMgr.getDecomposedLoc(Loc);
    auto FileStartLoc = SrcMgr.getLocForStartOfFile(FileItr->second);
    return FileStartLoc.getLocWithOffset(DecLoc.second);
  }

  /// Check that declarations which should not be distributed are not
  /// corrupted by distribution directives.
  void checkNotDistributedDecls(DeclarationSet NotDistrCanonicalDecls) {
    auto &SrcMgr = mCtx->getSourceManager();
    auto &Diags = mCtx->getDiagnostics();
    for (auto *VD : NotDistrCanonicalDecls)
      for (auto *Redecl : VD->getFirstDecl()->redecls()) {
        auto StartOfDecl = Redecl->getLocStart();
        // We have not inserted directives in a macro.
        if (StartOfDecl.isMacroID())
          continue;
        auto Filename = SrcMgr.getFilename(StartOfDecl);
        auto TfmFileItr = mTransformedFiles.find(Filename);
        if (TfmFileItr == mTransformedFiles.end())
          continue;
        auto DecLoc = SrcMgr.getDecomposedLoc(StartOfDecl);
        StartOfDecl = SrcMgr.getLocForStartOfFile(TfmFileItr->second).
          getLocWithOffset(DecLoc.second);
        // Whether a distribution pragma acts on this declaration?
        if (mInsertedDirs.count(StartOfDecl.getRawEncoding()))
          toDiag(Diags, StartOfDecl, diag::err_apc_not_distr_decl_directive);
      }
  }

  /// Insert inherit directive for all redeclarations of a specified function.
  void insertInherit(FunctionDecl *FD, const DeclarationInfoMap &Decls,
      ArrayRef<const DILocalVariable *> InheritArgs) {
    if (InheritArgs.empty())
      return;
    for (auto *Redecl : FD->getFirstDecl()->redecls()) {
      SmallString<64> Inherit;
      getPragmaText(DirectiveId::DvmInherit, Inherit);
      Inherit.pop_back();
      Inherit += "(";
      SmallVector<std::pair<SourceLocation, StringRef>, 8> UnnamedArgsInMacro;
      auto insert = [this, Redecl, &Inherit, &UnnamedArgsInMacro](
          const DILocalVariable *DIArg) {
        auto Param = Redecl->getParamDecl(DIArg->getArg() - 1);
        assert(Param && "Parameter must not be null!");
        if (Param->getName().empty()) {
          Inherit += DIArg->getName();
          auto Loc = Param->getLocation();
          if (Loc.isMacroID()) {
            UnnamedArgsInMacro.emplace_back(Loc, DIArg->getName());
          } else {
            SmallVector<char, 16> Name;
            // We add brackets due to the following case
            // void foo(double *A);
            // #define M
            // void foo(double *M);
            // Without brackets we obtain 'void foo(double *MA)' instead of
            // 'void foo(double *M(A))' and do not obtain 'void foo(double *A)'
            // after preprocessing.
            mRewriter->InsertTextBefore(Loc,
              ("(" + DIArg->getName() + ")").toStringRef(Name));
          }
        } else {
          Inherit += Param->getName();
        }
      };
      insert(InheritArgs.front());
      for (unsigned I = 1, EI = InheritArgs.size(); I < EI; ++I) {
        Inherit += ",";
        insert(InheritArgs[I]);
      }
      Inherit += ")\n";
      if (!UnnamedArgsInMacro.empty()) {
        auto &Diags = mCtx->getDiagnostics();
        toDiag(Diags, Redecl->getLocStart(),
          diag::err_apc_insert_dvm_directive) << StringRef(Inherit).trim();
        for (auto &Arg : UnnamedArgsInMacro)
          toDiag(Diags, Arg.first, diag::note_decl_insert_macro_prevent) <<
            Arg.second;
      }
      insertDataDirective(Redecl->getLocation(), Decls,
        Redecl->getLocStart(), Inherit);
    }
  }

  /// List of already transformed files.
  ///
  /// We should not transform different representations of the same files.
  /// For example, if a file has been included twice Rewriter does not allow
  /// to transform it twice.
  StringMap<FileID> mTransformedFiles;

  /// List of already inserted directives at specified locations.
  DenseMap<unsigned, std::string> mInsertedDirs;

  Rewriter *mRewriter;
  ASTContext *mCtx;
};

using APCDVMHWriterProvider = FunctionPassProvider<
  TransformationEnginePass,
  MemoryMatcherImmutableWrapper,
  ClangDIMemoryMatcherPass>;
}

char APCDVMHWriter::ID = 0;

INITIALIZE_PROVIDER_BEGIN(APCDVMHWriterProvider, "apc-dvmh-writer-provider",
  "DVMH Writer (APC, Provider")
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PROVIDER_END(APCDVMHWriterProvider, "apc-dvmh-writer-provider",
  "DVMH Writer (APC, Provider")

INITIALIZE_PASS_BEGIN(APCDVMHWriter, "apc-dvmh-writer",
  "DVMH Writer (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangDIGlobalMemoryMatcherPass)
  INITIALIZE_PASS_DEPENDENCY(ImmutableASTImportInfoPass)
  INITIALIZE_PASS_DEPENDENCY(APCDVMHWriterProvider)
  INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
INITIALIZE_PASS_END(APCDVMHWriter, "apc-dvmh-writer",
  "DVMH Writer (APC)", true, true)

ModulePass * llvm::createAPCDVMHWriter() { return new APCDVMHWriter; }

void APCDVMHWriter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangDIGlobalMemoryMatcherPass>();
  AU.addRequired<APCDVMHWriterProvider>();
  AU.addUsedIfAvailable<ImmutableASTImportInfoPass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

bool APCDVMHWriter::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto *TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }
  mCtx = &TfmCtx->getContext();
  mRewriter = &TfmCtx->getRewriter();
  APCDVMHWriterProvider::initialize<TransformationEnginePass>(
    [&M, TfmCtx](TransformationEnginePass &TEP) {
    TEP.setContext(M, TfmCtx);
  });
  auto &MatchInfo = getAnalysis<MemoryMatcherImmutableWrapper>().get();
  APCDVMHWriterProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MatchInfo](MemoryMatcherImmutableWrapper &Matcher) {
      Matcher.set(MatchInfo);
  });
  ASTImportInfo ImportStub;
  const auto *Import = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    Import = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  auto &APCRegion = APCCtx.getDefaultRegion();
  auto &DataDirs = APCRegion.GetDataDir();
  DenseSet<const AlignRule *> GlobalArrays;
  DenseMap<DISubprogram *, SmallVector<const AlignRule *, 16>> LocalVariables;
  for (auto &AR : DataDirs.alignRules) {
    auto *APCSymbol = AR.alignArray->GetDeclSymbol();
    assert(APCSymbol && "Symbol must not be null!");
    assert(APCSymbol->getMemory().isValid() && "Memory must be valid!");
    auto *DIVar = APCSymbol->getMemory().Var;
    if (isa<DIGlobalVariable>(DIVar)) {
      GlobalArrays.insert(&AR);
      continue;
    }
    assert(isa<DILocalVariable>(DIVar) && "It must be a local variable!");
    auto Scope = DIVar->getScope();
    while (Scope && !isa<DISubprogram>(Scope))
      Scope = Scope->getScope().resolve();
    assert(Scope && "Local variable must be declared in a subprogram!");
    auto LocalItr = LocalVariables.try_emplace(cast<DISubprogram>(Scope)).first;
    LocalItr->second.push_back(&AR);
  }
  DeclarationInfoMap Decls;
  DeclarationSet NotDistrCanonicalDecls;
  auto *Unit = mCtx->getTranslationUnitDecl();
  initializeDeclInfo(*Unit, *Import, Decls, NotDistrCanonicalDecls);
  TemplateInFileUsage Templates;
  auto insertAlignAndCollectTpl =
    [this, TfmCtx, Import, &Templates, &Decls](
      VarDecl &VD, const apc::AlignRule &AR) {
    auto DefLoc = insertAlignment(*Import, Decls, AR, &VD, *TfmCtx);
    // We should add declaration of template before 'align' directive.
    // So, we remember file with 'align' directive if this directive
    // has been successfully inserted.
    if (DefLoc.isValid()) {
      auto &SrcMgr = mCtx->getSourceManager();
      auto FID = SrcMgr.getFileID(DefLoc);
      auto TplItr = Templates.try_emplace(FID).first;
      TplItr->second.try_emplace(AR.alignWith);
    }
  };
  for (auto &Info : LocalVariables) {
    auto *F = M.getFunction(Info.first->getName());
    if (!F || F->getSubprogram() != Info.first)
      F = M.getFunction(Info.first->getLinkageName());
    assert(F && F->getSubprogram() == Info.first &&
      "LLVM IR function with attached metadata must not be null!");
    auto &Provider = getAnalysis<APCDVMHWriterProvider>(*F);
    auto &Matcher = Provider.get<ClangDIMemoryMatcherPass>().getMatcher();
    auto *FD =
      cast<FunctionDecl>(TfmCtx->getDeclForMangledName(F->getName()));
    assert(FD && "AST-level function declaration must not be null!");
    DeclarationInfoExtractor Visitor(Decls);
    Visitor.TraverseFunctionDecl(FD);
    SmallVector<DILocalVariable *, 8> InheritArgs;
    for (auto *AR : Info.second) {
      auto *APCSymbol = AR->alignArray->GetDeclSymbol();
      auto *DIVar = cast<DILocalVariable>(APCSymbol->getMemory().Var);
      auto Itr = Matcher.find<MD>(DIVar);
      assert(Itr != Matcher.end() && "Source-level location must be available!");
      if (DIVar->isParameter())
        InheritArgs.push_back(DIVar);
      else
        insertAlignAndCollectTpl(*Itr->get<AST>(), *AR);
      NotDistrCanonicalDecls.erase(Itr->get<AST>());
    }
    // TODO (kaniandr@gmail.com): check that there is no functions
    // without `inherit` directive
    insertInherit(FD, Decls, InheritArgs);
  }
  auto &GlobalMatcher = 
    getAnalysis<ClangDIGlobalMemoryMatcherPass>().getMatcher();
  for (auto *AR : GlobalArrays) {
    auto *APCSymbol = AR->alignArray->GetDeclSymbol();
    auto *DIVar = APCSymbol->getMemory().Var;
    auto Itr = GlobalMatcher.find<MD>(DIVar);
    assert(Itr != GlobalMatcher.end() &&
      "Source-level location must be available!");
    insertAlignAndCollectTpl(*Itr->get<AST>(), *AR);
    NotDistrCanonicalDecls.erase(Itr->get<AST>());
  }
  checkNotDistributedDecls(NotDistrCanonicalDecls);
  insertDistibution(APCRegion, DataDirs, *TfmCtx, Templates);
  for (auto &TplInfo : DataDirs.distrRules)
    GIP.getRawInfo().Identifiers.insert(TplInfo.first->GetShortName());
  return false;
}

SourceLocation APCDVMHWriter::insertAlignment(const  ASTImportInfo &Import,
    const DeclarationInfoMap &Decls, const apc::AlignRule &AR,
    const VarDecl *VD, TransformationContext &TfmCtx) {
  // Obtain `#pragma dvm array align` clause.
  SmallString<128> Align;
  getPragmaText(ClauseId::DvmAlign, Align);
  Align.pop_back();
  Align += "(";
  // Add dimension which should be aligned '... [...]...'
  for (std::size_t I = 0, EI = AR.alignRule.size(); I < EI; ++I) {
    assert((AR.alignRule[I].first == 0 || AR.alignRule[I].first == 1) &&
      AR.alignRule[I].second == 0 && "Invalid align rule!");
    Align += "[";
    if (AR.alignRule[I].first == 1 && AR.alignRule[I].second == 0)
     Align += AR.alignNames[I];
    Align += "]";
  }
  auto TplDimAR = extractTplDimsAlignmentIndexes(AR);
  // Add " ... with <template>[...]...[...]".
  Align += " with ";
  Align += AR.alignWith->GetShortName();
  for (auto DimARIdx : TplDimAR) {
    Align += "[";
    if (DimARIdx < TplDimAR.size()) {
      Align += genStringExpr(
        AR.alignNames[AR.alignRuleWith[DimARIdx].first],
        AR.alignRuleWith[DimARIdx].second);
    }
    Align += "]";
  }
  Align += ")\n";
  auto &SrcMgr = mCtx->getSourceManager();
  SourceLocation DefinitionLoc;
  auto *VarDef = VD->getDefinition();
  if (VarDef) {
    DefinitionLoc = VarDef->getLocation();
    auto StartOfDecl = VarDef->getLocStart();
    insertDataDirective(DefinitionLoc, Decls, StartOfDecl, Align);
  }
  // Insert 'align' directive before a variable definition (if it is available)
  // and insert 'array' directive before redeclarations of a variable.
  SmallString<16> Array;
  getPragmaText(DirectiveId::DvmArray, Array);
  for (auto *Redecl : VD->getFirstDecl()->redecls()) {
    auto StartOfDecl = Redecl->getLocStart();
    auto RedeclLoc = Redecl->getLocation();
    switch (Redecl->isThisDeclarationADefinition()) {
    case VarDecl::Definition: break;
    case VarDecl::DeclarationOnly:
      insertDataDirective(RedeclLoc, Decls, StartOfDecl, Array);
      break;
    case VarDecl::TentativeDefinition:
      if (DefinitionLoc.isInvalid()) {
        auto FID = SrcMgr.getFileID(StartOfDecl);
        bool IsInclude = SrcMgr.getDecomposedIncludedLoc(FID).first.isValid();
        if (IsInclude) {
          insertDataDirective(RedeclLoc, Decls, StartOfDecl, Array);
        } else {
          DefinitionLoc = Redecl->getLocation();
          insertDataDirective(RedeclLoc, Decls, StartOfDecl, Align);
        }
      } else {
        DefinitionLoc = Redecl->getLocation();
        insertDataDirective(RedeclLoc, Decls, StartOfDecl, Align);
      }
      break;
    }
    auto RedeclLocItr = Import.RedeclLocs.find(Redecl);
    if (RedeclLocItr != Import.RedeclLocs.end()) {
      auto &Locs = RedeclLocItr->second.find(RedeclLoc);
      auto &StartLocs = RedeclLocItr->second.find(StartOfDecl);
      for (std::size_t I = 0, EI = Locs.size(); I < EI; ++I) {
        if (Locs[I] == RedeclLoc)
          continue;
        insertDataDirective(Locs[I], Decls, StartLocs[I], Array);
      }
    }
  }
  if (DefinitionLoc.isInvalid()) {
    toDiag(mCtx->getDiagnostics(), VD->getLocation(),
      diag::err_apc_insert_dvm_directive) << StringRef(Align).trim();
    toDiag(mCtx->getDiagnostics(), diag::note_apc_no_proper_definition) <<
      VD->getName();
  }
  return DefinitionLoc;
}

void APCDVMHWriter::insertDistibution(const apc::ParallelRegion &Region,
    const apc::DataDirective &DataDirs, TransformationContext &TfmCtx,
    TemplateInFileUsage &Templates) {
  auto &Rewriter = TfmCtx.getRewriter();
  auto &SrcMgr = Rewriter.getSourceMgr();
  auto &LangOpts = Rewriter.getLangOpts();
  auto &Diags = TfmCtx.getContext().getDiagnostics();
  SmallPtrSet<apc::Array *, 8> InsertedTemplates;
  for (auto &File : Templates) {
    auto PreInfo =
      Lexer::ComputePreamble(SrcMgr.getBufferData(File.first), LangOpts);
    // Process templates which are used in a current file.
    for (std::size_t DistrRuleIdx = 0,
        DistrRuleEIdx = DataDirs.distrRules.size();
        DistrRuleIdx < DistrRuleEIdx; ++DistrRuleIdx) {
      auto &AllTplDistrRules = DataDirs.distrRules[DistrRuleIdx];
      auto TplUsageItr = File.second.find(AllTplDistrRules.first);
      if (TplUsageItr == File.second.end())
        continue;
      auto *Tpl = TplUsageItr->first;
      auto &TplInfo = TplUsageItr->second;
      SmallString<256> Distribute;
      // Obtain "#pragma dvm template"
      getPragmaText(DirectiveId::DvmTemplate, Distribute);
      Distribute.pop_back(); Distribute += " ";
      // Add size of each template dimension to pragma: "... [Size] ..."
      auto &DimSizes = Tpl->GetSizes();
      for (std::size_t DimIdx = 0, DimIdxE = Tpl->GetDimSize();
           DimIdx < DimIdxE; ++DimIdx) {
        assert(DimSizes[DimIdx].first == 0 &&
          "Lower dimension bound must be 0 for C language!");
        Distribute += "[";
        Distribute += Twine(DimSizes[DimIdx].second).str();
        Distribute += "]";
      }
      // Add distribution rules according to current distribution variant.
      Distribute += " distribute ";
      auto &DistrVariant = Region.GetCurrentVariant();
      assert(DistrVariant[DistrRuleIdx] < AllTplDistrRules.second.size() &&
        "Variant index must be less than number of variants!");
      auto &DR = AllTplDistrRules.second[DistrVariant[DistrRuleIdx]];
      for (auto Kind : DR.distRule) {
        switch (Kind) {
        case BLOCK: Distribute += "[block]"; break;
        case NONE: Distribute += "[]"; break;
        default:
          llvm_unreachable("Unknown distribution rule!");
          Distribute += "[]"; break;
        }
      }
      Distribute += "\n";
      auto Where =
        SrcMgr.getLocForStartOfFile(File.first).getLocWithOffset(PreInfo.Size);
      // TODO (kaniandr@gmail.com): do not insert directive in include file
      // if some inclusion locations may be in a local scope. Such check is
      // not implemented, hence we conservatively disable insertion of directive
      // in an include file.
      if (SrcMgr.getDecomposedIncludedLoc(File.first).first.isValid()) {
        toDiag(Diags, Where, diag::err_apc_insert_dvm_directive) <<
          StringRef(Distribute).trim();
        toDiag(Diags, Where, diag::note_apc_insert_include_prevent);
      }
      // Use `extern` in to avoid variable redefinition.
      if (TplInfo.HasDefinition)
        Distribute += "extern";
      else
        TplInfo.HasDefinition = true;
      Distribute += "void *";
      Distribute += Tpl->GetShortName();
      Distribute += ";\n\n";
      // Insert at the end of preamble.
      Where = getLocationToTransform(Where);
      assert(Where.isFileID() && "Location must not be in macro!");
      Rewriter.InsertTextBefore(Where, Distribute);
      mTransformedFiles.try_emplace(SrcMgr.getFilename(Where), File.first);
      InsertedTemplates.insert(Tpl);
    }
  }
  if (DataDirs.distrRules.size() != InsertedTemplates.size())
    for (auto &TplInfo : DataDirs.distrRules)
      if (!InsertedTemplates.count(TplInfo.first))
        toDiag(Diags, diag::err_apc_insert_template) <<
          TplInfo.first->GetShortName();
}
