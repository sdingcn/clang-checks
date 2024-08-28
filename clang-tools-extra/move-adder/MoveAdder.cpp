#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/CFG.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "../clang-tidy/utils/ExprSequence.h"
#include "../clang-tidy/utils/Matchers.h"
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

class TraverseFunctionVisitor
    : public RecursiveASTVisitor<TraverseFunctionVisitor> {
public:
  explicit TraverseFunctionVisitor(const ValueDecl *V)
    : VarDecl(V), Count(0) {}
  bool VisitDeclRefExpr(DeclRefExpr *VarRef) {
    if (VarRef->getDecl() == VarDecl) {
      Count++;
    }
    return true;
  }
  int getCount() {
    return Count;
  }
private:
  const ValueDecl *VarDecl;
  int Count;
};

/******************************************************
 * core functions
 ******************************************************/

bool isMovable(const FunctionDecl *Fun, const DeclRefExpr *VarRef, ASTContext *Ctx) {
  bool IsLocal = VarRef->getDecl()->getDeclContext()->isFunctionOrMethod();
  bool IsModifiable = !(VarRef->getDecl()->getType()->isLValueReferenceType());
  TraverseFunctionVisitor Visitor(VarRef->getDecl());
  Visitor.TraverseDecl(const_cast<FunctionDecl*>(Fun));
  bool IsUniqueUse = Visitor.getCount() == 1;
  // TODO: check that adding std::move does not modify surrounding overloading resolutions
  return IsLocal && IsModifiable && IsUniqueUse;
}

std::pair<std::string, std::pair<int, int>> getMoveLoc(
  const clang::DeclRefExpr *VarRef, ASTContext *Ctx
) {
  FullSourceLoc Loc = Ctx->getFullLoc(VarRef->getBeginLoc());
  if (Loc.isValid()) {
    auto FName = Loc.getFileEntry()->tryGetRealPathName().str();
    auto FPos = std::make_pair(Loc.getSpellingLineNumber(), Loc.getSpellingColumnNumber());
    return std::make_pair(FName, FPos);
  } else {
    return std::make_pair("N/A", std::make_pair(-1, -1));
  }
}

/******************************************************
 * main entry
 ******************************************************/

class CopyHandlerGeneric : public MatchFinder::MatchCallback {
public:
  virtual void moveOp(std::pair<std::string, std::pair<int, int>> Loc , const DeclRefExpr* VarRef);
  
  virtual void run(const MatchFinder::MatchResult &Result) override {
    const auto *Fun = Result.Nodes.getNodeAs<FunctionDecl>("node[containing-function]");
    if (!Fun->isThisDeclarationADefinition()) { // not sure whether this is necessary
      return;
    }
    const auto *Call = Result.Nodes.getNodeAs<Expr>("node[copy-construction]");
    if (!Call) {
      Call = Result.Nodes.getNodeAs<Expr>("node[copy-assignment]");
    }
    const auto *VarRef = Result.Nodes.getNodeAs<DeclRefExpr>("node[variable]");
    auto Ctx = Result.Context;
    if (isMovable(Fun, VarRef, Ctx)) {
      auto Loc = getMoveLoc(VarRef, Ctx);
      moveOp(Loc, VarRef);
    }
  }
};

class CopyHandler : public CopyHandlerGeneric {
public:
  virtual void moveOp(std::pair<std::string, std::pair<int, int>> Loc, const DeclRefExpr* VarRef) {
    llvm::outs() << "[MoveAdder]: Variable "
                   << VarRef->getDecl()->getQualifiedNameAsString()
                   << " at location "
                   << "(" << Loc.first << ", " << Loc.second.first << ", " << Loc.second.second << ")"
                   << " is movable.\n";
  }
};

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
  if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  auto VarHasMoveCtorMatcher = 
    hasType(
      hasUnqualifiedDesugaredType(
        recordType(
          hasDeclaration(
            cxxRecordDecl(
              hasDescendant(
                cxxConstructorDecl(
                  unless(isImplicit()),
                  unless(isDeleted()),
                  isMoveConstructor()
                )
              )
            )
          )
        )
      )
    );
  auto VarHasMoveOpMatcher =
    hasType(
      hasUnqualifiedDesugaredType(
        recordType(
          hasDeclaration(
            cxxRecordDecl(
              hasDescendant(
                cxxMethodDecl(
                  unless(isImplicit()),
                  unless(isDeleted()),
                  isMoveAssignmentOperator()
                )
              )
            )
          )
        )
      )
    );
  auto CopyConstructionMatcher =
    cxxConstructExpr(
      hasDeclaration(cxxConstructorDecl(isCopyConstructor())),
      forEachArgumentWithParam(
        declRefExpr(VarHasMoveCtorMatcher).bind("node[variable]"),
        parmVarDecl()
      ),
      hasAncestor(functionDecl(isDefinition()).bind("node[containing-function]")),
      hasAncestor(compoundStmt()) // make sure it's in the function body
    ).bind("node[copy-construction]");
  auto CopyAssignmentMatcher =
    cxxOperatorCallExpr(
      hasDeclaration(cxxMethodDecl(isCopyAssignmentOperator())),
      forEachArgumentWithParam(
        declRefExpr(VarHasMoveOpMatcher).bind("node[variable]"),
        parmVarDecl()
      ),
      hasAncestor(functionDecl(isDefinition()).bind("node[containing-function]")),
      hasAncestor(compoundStmt()) // make sure it's in the function body
    ).bind("node[copy-assignment]");
  MatchFinder Finder;
  CopyHandler Handler;
  Finder.addMatcher(CopyConstructionMatcher, &Handler);
  Finder.addMatcher(CopyAssignmentMatcher, &Handler);
  return Tool.run(newFrontendActionFactory(&Finder).get());
}