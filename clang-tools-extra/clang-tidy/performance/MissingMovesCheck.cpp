//===--- MissingMovesCheck.cpp - clang-tidy -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/*

## algorithm

### a variable's movability

+ is a local variable or local rvalue reference (both including function parameter)
+ is not used later in the current function's control-flow graph
+ its type has move constructor and move assignment operator

### stdlib calls

rationale: we precisely know the functionality of those overloading candidates

for callsite of stdlib functions with overloading options: # e.g. push_back(const T&) and push_back(T&&)
    if argument is a movable variable:
        add std::move

### other cases

for CXXConstructExpr with (const C&):
    if expr's parent callees all have unique definitions:
        if argument is a movable variable:
            add std::move

for CXXOperatorCallExpr with operator=(const C&):
    if expr's parent callees all have unique definitions:
        if argument is a movable variable:
            add std::move

*/

#include "MissingMovesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/CFG.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "../utils/ExprSequence.h"
#include "../utils/Matchers.h"
#include <optional>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>

using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

namespace clang::tidy::performance {

bool isStandardSmartPointer(const ValueDecl *VD) {
  const Type *TheType = VD->getType().getNonReferenceType().getTypePtrOrNull();
  if (!TheType)
    return false;

  const CXXRecordDecl *RecordDecl = TheType->getAsCXXRecordDecl();
  if (!RecordDecl)
    return false;

  const IdentifierInfo *ID = RecordDecl->getIdentifier();
  if (!ID)
    return false;

  StringRef Name = ID->getName();
  if (Name != "unique_ptr" && Name != "shared_ptr" && Name != "weak_ptr")
    return false;

  return RecordDecl->getDeclContext()->isStdNamespace();
}

// helper code
namespace {

/// Finds uses of a variable after a copy (and maintains state required by the
/// various internal helper functions).
class UseAfterCopyFinder {
public:
  UseAfterCopyFinder(ASTContext *TheContext) : Context(TheContext) {}

  // Within the given code block, returns whether a use-after-copy was found.
  bool find(Stmt *CodeBlock, const Expr *Call,
            const ValueDecl *CopiedVariable) {
    // Generate the CFG manually instead of through an AnalysisDeclContext because
    // it seems the latter can't be used to generate a CFG for the body of a
    // lambda.
    //
    // We include implicit and temporary destructors in the CFG so that
    // destructors marked [[noreturn]] are handled correctly in the control flow
    // analysis. (These are used in some styles of assertion macros.)
    CFG::BuildOptions Options;
    Options.AddImplicitDtors = true;
    Options.AddTemporaryDtors = true;
    std::unique_ptr<CFG> TheCFG =
        CFG::buildCFG(nullptr, CodeBlock, Context, Options);
    if (!TheCFG)
      return false;

    Sequence = std::make_unique<ExprSequence>(TheCFG.get(), CodeBlock, Context);
    BlockMap = std::make_unique<StmtToBlockMap>(TheCFG.get(), Context);
    Visited.clear();

    const CFGBlock *Block = BlockMap->blockContainingStmt(Call);
    if (!Block) {
      // This can happen if Call is in a constructor initializer, which is
      // not included in the CFG because the CFG is built only from the function
      // body.
      Block = &TheCFG->getEntry();
    }

    return findInternal(Block, Call, CopiedVariable);
  }

private:
  bool findInternal(const CFGBlock *Block, const Expr *Call,
                    const ValueDecl *CopiedVariable) {
    if (Visited.count(Block))
      return false;

    // Mark the block as visited (except if this is the block containing the
    // call and it's being visited the first time).
    if (!Call)
      Visited.insert(Block);

    // Get all uses and reinits in the block.
    llvm::SmallVector<const DeclRefExpr *, 1> Uses;
    llvm::SmallPtrSet<const Stmt *, 1> Reinits;
    getUsesAndReinits(Block, CopiedVariable, &Uses, &Reinits);

    // Ignore all reinitializations where the call potentially comes after the
    // reinit.
    llvm::SmallVector<const Stmt *, 1> ReinitsToDelete;
    for (const Stmt *Reinit : Reinits) {
      if (Call && Sequence->potentiallyAfter(Call, Reinit))
        ReinitsToDelete.push_back(Reinit);
    }
    for (const Stmt *Reinit : ReinitsToDelete) {
      Reinits.erase(Reinit);
    }

    // Find all uses that potentially come after the call.
    for (const DeclRefExpr *Use : Uses) {
      if (!Call || Sequence->potentiallyAfter(Use, Call)) {
        // Does the use have a saving reinit? A reinit is saving if it definitely
        // comes before the use, i.e. if there's no potential that the reinit is
        // after the use.
        bool HaveSavingReinit = false;
        for (const Stmt *Reinit : Reinits) {
          if (!Sequence->potentiallyAfter(Reinit, Use))
            HaveSavingReinit = true;
        }

        if (!HaveSavingReinit) {
          return true; // this is a REAL use after copy
        }
      }
    }

    // If the object wasn't reinitialized, call ourselves recursively on all
    // successors.
    if (Reinits.empty()) {
      for (const auto &Succ : Block->succs()) {
        if (Succ && findInternal(Succ, nullptr, CopiedVariable))
          return true;
      }
    }

    return false;
  }

  void getUsesAndReinits(const CFGBlock *Block, const ValueDecl *CopiedVariable,
                         llvm::SmallVectorImpl<const DeclRefExpr *> *Uses,
                         llvm::SmallPtrSetImpl<const Stmt *> *Reinits) {
    llvm::SmallPtrSet<const DeclRefExpr *, 1> DeclRefs;
    llvm::SmallPtrSet<const DeclRefExpr *, 1> ReinitDeclRefs;

    getDeclRefs(Block, CopiedVariable, &DeclRefs);
    getReinits(Block, CopiedVariable, Reinits, &ReinitDeclRefs);

    // All references to the variable that aren't reinitializations are uses.
    Uses->clear();
    for (const DeclRefExpr *DeclRef : DeclRefs) {
      if (!ReinitDeclRefs.count(DeclRef))
        Uses->push_back(DeclRef);
    }

    // Sort the uses by their occurrence in the source code.
    llvm::sort(*Uses, [](const DeclRefExpr *D1, const DeclRefExpr *D2) {
      return D1->getExprLoc() < D2->getExprLoc();
    });
  }

  void getDeclRefs(const CFGBlock *Block, const Decl *CopiedVariable,
                   llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs) {

    // Matches nodes that are
    // - Part of a decltype argument or class template argument (we check this by
    //   seeing if they are children of a TypeLoc), or
    // - Part of a function template argument (we check this by seeing if they are
    //   children of a DeclRefExpr that references a function template).
    // DeclRefExprs that fulfill these conditions should not be counted as a use or
    // copy.
    auto inDecltypeOrTemplateArg = anyOf(hasAncestor(typeLoc()),
                hasAncestor(declRefExpr(
                    to(functionDecl(ast_matchers::isTemplateInstantiation())))),
                hasAncestor(expr(matchers::hasUnevaluatedContext())));

    DeclRefs->clear();
    for (const auto &Elem : *Block) {
      std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
      if (!S)
        continue;

      auto AddDeclRefs = [this, Block,
                          DeclRefs](const ArrayRef<BoundNodes> Matches) {
        for (const auto &Match : Matches) {
          const auto *DeclRef = Match.getNodeAs<DeclRefExpr>("declref");
          const auto *Operator = Match.getNodeAs<CXXOperatorCallExpr>("operator");
          if (DeclRef && BlockMap->blockContainingStmt(DeclRef) == Block) {
            // Ignore uses of a standard smart pointer that don't dereference the
            // pointer.
            if (Operator || !isStandardSmartPointer(DeclRef->getDecl())) {
              DeclRefs->insert(DeclRef);
            }
          }
        }
      };

      auto DeclRefMatcher = declRefExpr(hasDeclaration(equalsNode(CopiedVariable)),
                                        unless(inDecltypeOrTemplateArg))
                                .bind("declref");

      AddDeclRefs(match(traverse(TK_AsIs, findAll(DeclRefMatcher)), *S->getStmt(),
                        *Context));
      AddDeclRefs(match(findAll(cxxOperatorCallExpr(
                                    hasAnyOverloadedOperatorName("*", "->", "[]"),
                                    hasArgument(0, DeclRefMatcher))
                                    .bind("operator")),
                        *S->getStmt(), *Context));
    }
  }

  void getReinits(const CFGBlock *Block, const ValueDecl *CopiedVariable,
                  llvm::SmallPtrSetImpl<const Stmt *> *Stmts,
                  llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs) {
    auto DeclRefMatcher =
        declRefExpr(hasDeclaration(equalsNode(CopiedVariable))).bind("declref");

    auto StandardContainerTypeMatcher = hasType(hasUnqualifiedDesugaredType(
        recordType(hasDeclaration(cxxRecordDecl(hasAnyName(
            "::std::basic_string", "::std::vector", "::std::deque",
            "::std::forward_list", "::std::list", "::std::set", "::std::map",
            "::std::multiset", "::std::multimap", "::std::unordered_set",
            "::std::unordered_map", "::std::unordered_multiset",
            "::std::unordered_multimap"))))));

    auto StandardSmartPointerTypeMatcher = hasType(hasUnqualifiedDesugaredType(
        recordType(hasDeclaration(cxxRecordDecl(hasAnyName(
            "::std::unique_ptr", "::std::shared_ptr", "::std::weak_ptr"))))));

    // Matches different types of reinitialization.
    auto ReinitMatcher =
        stmt(anyOf(
                // Assignment. In addition to the overloaded assignment operator,
                // test for built-in assignment as well, since template functions
                // may be instantiated to use std::move() on built-in types.
                binaryOperation(hasOperatorName("="), hasLHS(DeclRefMatcher)),
                // Declaration. We treat this as a type of reinitialization too,
                // so we don't need to treat it separately.
                declStmt(hasDescendant(equalsNode(CopiedVariable))),
                // clear() and assign() on standard containers.
                cxxMemberCallExpr(
                    on(expr(DeclRefMatcher, StandardContainerTypeMatcher)),
                    // To keep the matcher simple, we check for assign() calls
                    // on all standard containers, even though only vector,
                    // deque, forward_list and list have assign(). If assign()
                    // is called on any of the other containers, this will be
                    // flagged by a compile error anyway.
                    callee(cxxMethodDecl(hasAnyName("clear", "assign")))),
                // reset() on standard smart pointers.
                cxxMemberCallExpr(
                    on(expr(DeclRefMatcher, StandardSmartPointerTypeMatcher)),
                    callee(cxxMethodDecl(hasName("reset")))),
                // Methods that have the [[clang::reinitializes]] attribute.
                cxxMemberCallExpr(
                    on(DeclRefMatcher),
                    callee(cxxMethodDecl(hasAttr(clang::attr::Reinitializes)))),
                // Passing variable to a function as a non-const pointer.
                callExpr(forEachArgumentWithParam(
                    unaryOperator(hasOperatorName("&"),
                                  hasUnaryOperand(DeclRefMatcher)),
                    unless(parmVarDecl(hasType(pointsTo(isConstQualified())))))),
                // Passing variable to a function as a non-const lvalue reference
                // (unless that function is std::move()).
                callExpr(forEachArgumentWithParam(
                              traverse(TK_AsIs, DeclRefMatcher),
                              unless(parmVarDecl(hasType(
                                  references(qualType(isConstQualified())))))),
                          unless(callee(functionDecl(hasName("::std::move")))))))
            .bind("reinit");

    Stmts->clear();
    DeclRefs->clear();
    for (const auto &Elem : *Block) {
      std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
      if (!S)
        continue;

      SmallVector<BoundNodes, 1> Matches =
          match(findAll(ReinitMatcher), *S->getStmt(), *Context);

      for (const auto &Match : Matches) {
        const auto *TheStmt = Match.getNodeAs<Stmt>("reinit");
        const auto *TheDeclRef = Match.getNodeAs<DeclRefExpr>("declref");
        if (TheStmt && BlockMap->blockContainingStmt(TheStmt) == Block) {
          Stmts->insert(TheStmt);

          // We count DeclStmts as reinitializations, but they don't have a
          // DeclRefExpr associated with them -- so we need to check 'TheDeclRef'
          // before adding it to the set.
          if (TheDeclRef)
            DeclRefs->insert(TheDeclRef);
        }
      }
    }
  }

  ASTContext *Context;
  std::unique_ptr<ExprSequence> Sequence;
  std::unique_ptr<StmtToBlockMap> BlockMap;
  llvm::SmallPtrSet<const CFGBlock *, 8> Visited;
};

} // namespace

void MissingMovesCheck::registerMatchers(MatchFinder *Finder) {
  auto hasMoveCtor = 
    hasType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(cxxRecordDecl(
        hasDescendant(
          cxxConstructorDecl(unless(isImplicit()), unless(isDeleted()), isMoveConstructor())
        )
      )))
    ));
  auto hasMoveOp =
    hasType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(cxxRecordDecl(
        hasDescendant(
          cxxMethodDecl(unless(isImplicit()), unless(isDeleted()), isMoveAssignmentOperator())
        )
      )))
    ));
  auto StdLibCallMatcher =
    cxxMemberCallExpr(
      on(expr(hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(cxxRecordDecl(hasAnyName(
        "::std::vector", "::std::deque", "::std::set",
        "::std::unordered_set", "::std::multiset", "::std::unordered_multiset")))))))),
      forEachArgumentWithParam(declRefExpr(hasMoveCtor).bind("node[variable]"), parmVarDecl()),
      callee(cxxMethodDecl(hasAnyName("push_back", "push_front", "insert"))),
      hasAncestor(functionDecl().bind("node[containing-function]"))).bind("node[standard-library-call]");
  auto CopyCtorMatcher =
    cxxConstructExpr(
      hasDeclaration(cxxConstructorDecl(isCopyConstructor())),
      forEachArgumentWithParam(declRefExpr(hasMoveCtor).bind("node[variable]"), parmVarDecl()),
      hasAncestor(functionDecl().bind("node[containing-function]"))).bind("node[copy-construction]");
  auto CopyAssignMatcher =
    cxxOperatorCallExpr(
      hasDeclaration(cxxMethodDecl(isCopyAssignmentOperator())),
      forEachArgumentWithParam(declRefExpr(hasMoveOp).bind("node[variable]"), parmVarDecl()),
      hasAncestor(functionDecl().bind("node[containing-function]"))).bind("node[copy-assignment]");
  Finder->addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, StdLibCallMatcher), this);
  Finder->addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, CopyCtorMatcher), this);
  Finder->addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, CopyAssignMatcher), this);
}

// helpers
namespace {
  bool isMovable(Stmt *fun, const Expr *call, const DeclRefExpr *var, ASTContext *ctx) {
    bool isLocal = var->getDecl()->getDeclContext()->isFunctionOrMethod();
    bool isModifiable = !(var->getDecl()->getType()->isLValueReferenceType());
    bool isLastUse = !UseAfterCopyFinder(ctx).find(fun, call, var->getDecl());
    // check not modifying overloading resolution
    return isLocal && isModifiable && isLastUse;
  }
  bool contains(const std::string &s, const std::string &t) {
    return s.find(t) != std::string::npos;
  }
  bool upLoop(const clang::Stmt &stmt, ASTContext *ctx) {
    // add adaptor for clang::Decl
    std::string name(stmt.getStmtClassName());
    if (name == std::string("ForStmt") || name == std::string("WhileStmt") || name == std::string("DoStmt"))
      return true;
    bool hasLoop = false;
    auto parents = ctx->getParents(stmt);
    for (auto it = parents.begin(); it != parents.end(); it++) {
      const clang::Stmt *parent = it->get<clang::Stmt>();
      if (parent) {
        auto sub = upLoop(*parent, ctx);
        hasLoop |= sub;
      }
    }
    return hasLoop;
  }
  int score(const DeclRefExpr *var, ASTContext *ctx) {
    int s = 0;
    // containers
    auto decl = var->getType()->getUnqualifiedDesugaredType()->getAsRecordDecl();
    for (auto &n : std::vector<std::string>{"vector", "deque", "list", "set", "map"}) {
      if (contains(decl->getNameAsString(), n)) {
        s += 2;
        break;
      }
    }
    // inside loop(s)
    if (upLoop(*var, ctx)) {
      s += 1;
    }
    return s;
  }
}

void MissingMovesCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *fun = Result.Nodes.getNodeAs<FunctionDecl>("node[containing-function]");
  const auto *call = Result.Nodes.getNodeAs<Expr>("node[standard-library-call]");
  if (!call) call = Result.Nodes.getNodeAs<Expr>("node[copy-construction]");
  if (!call) call = Result.Nodes.getNodeAs<Expr>("node[copy-assignment]");
  const auto *var = Result.Nodes.getNodeAs<DeclRefExpr>("node[variable]");

  // var->printPretty();

  auto ctx = Result.Context;

  if (isMovable(fun->getBody(), call, var, ctx)) {
    std::ostringstream oss;
    // the score is an intuitive measure of how costly the copy is
    // currently it is considered as costly whenever it is
    // (1) copying a standard container
    // (2) occurring inside loop(s)
    oss << var->getDecl()->getQualifiedNameAsString() << " is MOVABLE with score " << score(var, ctx) << ".";
    std::string fix = "std::move(" + var->getDecl()->getNameAsString() + ")";
    this->diag(call->getExprLoc(), oss.str()) << FixItHint::CreateReplacement(var->getSourceRange(), fix);
  }
}

} // namespace clang::tidy::performance
