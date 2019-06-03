//===--- ASTImportInfo.h - AST Import Process Information--------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines storage to access import process information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_AST_IMPORT_INFO_H
#define TSAR_AST_IMPORT_INFO_H

#include "tsar_pass.h"
#include <clang/AST/Decl.h>
#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>
#include <vector>

namespace clang {
class ASTImporter;
class Decl;
}

namespace tsar {
/// Extended information about the import process.
struct ASTImportInfo {
  /// Represent synonyms for a locations attached to a single declaration.
  ///
  /// Importer merges imported external declarations to the existing one. So,
  /// the information of locations of an original declaration may be lost.
  /// For example, Import(FileID of From) != FileID of To. In this case it is
  /// not possible to find include which makes the From location visible at
  /// some point (such information is necessary for example in ClangInliner).
  class MergedLocations {
  public:
    using RedeclLocList = std::vector<clang::SourceLocation>;

    /// Initialize list of merged locations, 'ToLocs' is a list of all
    /// locations attached to a declaration which is a target of merge action.
    explicit MergedLocations(llvm::ArrayRef<clang::SourceLocation> ToLocs) {
      for (auto Loc : ToLocs)
        mRedeclLocs.emplace_back(1, Loc);
    }

    /// Return list of locations related to redeclarations of a specified
    /// location. This list also contains original location `Loc`.
    const RedeclLocList & find(clang::SourceLocation Loc) const {
      for (auto &Locs : mRedeclLocs)
        if (Locs[0] == Loc)
          return Locs;
    }

    /// Add list of all locations attached to single redeclaration for a current
    /// declaration.
    void push_back(llvm::ArrayRef<clang::SourceLocation> MergedLocs) {
      assert(MergedLocs.size() == mRedeclLocs.size() &&
        "Number of attached locations differs for different redeclarations!");
      for (std::size_t I = 0, EI = MergedLocs.size(); I < EI; ++I)
        mRedeclLocs[I].push_back(MergedLocs[I]);
    }

  private:
    llvm::SmallVector<RedeclLocList, 5> mRedeclLocs;
  };

  using RedeclLocMap = llvm::DenseMap<clang::Decl *, MergedLocations>;
  RedeclLocMap RedeclLocs;

  /// True if import has been performed.
  bool WasImport = false;
};
}

namespace llvm {
/// Gives access to the import process information.
class ImmutableASTImportInfoPass :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;
  ImmutableASTImportInfoPass(const tsar::ASTImportInfo *Info = nullptr) :
    ImmutablePass(ID), mImportInfo(Info) {
    assert(Info && "Import information must not be null!");
    initializeImmutableASTImportInfoPassPass(*PassRegistry::getPassRegistry());
  }
  const tsar::ASTImportInfo & getImportInfo() const noexcept {
    return *mImportInfo;
  }
private:
  const tsar::ASTImportInfo *mImportInfo;
};
}
#endif//TSAR_AST_IMPORT_INFO_H
