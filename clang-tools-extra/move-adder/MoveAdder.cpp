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
#include "clang/Rewrite/Core/Rewriter.h"
#include <vector>
#include <string>
#include <utility>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <random>
#include <fstream>

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

// remove all sugars, qualifiers, and references
// TODO: clean up this part
QualType getCleanType(QualType t) {
  return t.getCanonicalType().getUnqualifiedType().getNonReferenceType();
}

bool hasNonTrivialClass(const DeclRefExpr *VarRef) {
  auto T = getCleanType(VarRef->getType());
  auto C = T->getAsCXXRecordDecl();
  if (C) {
    for (auto FieldIter = C->field_begin(); FieldIter != C->field_end(); FieldIter++) {
      FieldDecl *FP = *FieldIter;
      QualType FT = FP->getType();
      // TODO: add custom checks to find out "interesting non-trivial classes"
    }
    return (
      C->hasNonTrivialMoveConstructor() &&
      C->hasNonTrivialMoveAssignment() &&
      C->hasNonTrivialDestructor()
    );
  } else {
    return false;
  }
}

struct MoveInfo {
  std::string File;
  std::pair<int, int> Loc;
  std::string varName;
};

class CopyHandler : public MatchFinder::MatchCallback {
public:
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
    if (hasNonTrivialClass(VarRef) && isMovable(Fun, VarRef, Ctx)) {
      auto FileAndLoc = getMoveLoc(VarRef, Ctx);
      movables.push_back(MoveInfo{FileAndLoc.first, FileAndLoc.second, VarRef->getNameInfo().getAsString()});
      // llvm::outs() << "[MoveAdder]: Variable "
      //              << VarRef->getDecl()->getQualifiedNameAsString()
      //              << " at location "
      //              << "(" << Loc.first << ", " << Loc.second.first << ", " << Loc.second.second << ")"
      //              << " is movable.\n";
    }
  }
public:
  std::vector<MoveInfo> movables;
};

/*
---
for file in files.keys():
  std::vector<VarRef> Moves = files[file]
  Rewriter(File)
  applyRewrites(all moves in file)
  write to file
*/

void applyMoves(const std::vector<MoveInfo> &movables) {
  // Maps file names/line numbers to move locations
  std::map<std::string,
    std::map<int, std::vector<std::pair<int, int>>>
  > M;
  for (auto &m : movables) {
    // cppreference: [] inserts value_type(key, T()) if the key does not exist.
    M[m.File][m.Loc.first].push_back(std::make_pair(m.Loc.second, m.varName.size()));
  }
  for (auto &p1 : M) {
    for (auto &p2 : p1.second) {
      std::sort(p2.second.begin(), p2.second.end(),
        [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
          return a.first > b.first;
        }
      );
    }
  }
  for (auto &p1 : M) {
    std::string fname = p1.first;
    std::fstream fs(fname, std::ios::in | std::ios::out);
    std::vector<std::string> lines;
    std::string line;
    while (getline(fs, line)) {
      lines.push_back(line);
    }
    for (auto &p2 : p1.second) {
      int line = p2.first;
      for (auto &p3 : p2.second) {
        int column = p3.first;
        int varLen = p3.second;
        int endPos = column + varLen;
        lines[line].insert(endPos, ")");
        lines[line].insert(column, "std::move(");
      }
    }
    fs << "#include <utility> \n";
    for (auto &l : lines) {
      fs << l << '\n';
    }
  }
/*
  for (auto moveable = movables.begin(); moveable < movables.end(); moveable++) {
    if (moveInfoMap.contains((*moveable).File)) {
      
    }
  }
  Rewriter R;
  R.setSourceMgr((*moveable).Ctx->getSourceManager(), (*moveable).Ctx->getLangOpts());
  for (auto moveable = movables.begin(); moveable < movables.end(); moveable++) {
    
    if (R.isRewritable) {
      DeclRefExpr *VarRef;
      auto begin = VarRef->getBeginLoc();
      auto end = VarRef->getEndLoc();
      R.InsertTextBefore(begin, "std::move(");
      R.InsertTextAfter(end, ")");
    }
  }
  R.getEditBuffer(R.getSourceMgr().getMainFileID()).write(OutFile);
*/
}

void resetMoves(const std::vector<MoveInfo> &movables) {
  // TOOD initialize git repo in concept-synth
  system("git restore");
}

time_t callTest(std::string testCmd) {
  // TODO: re-compile the project
  time_t diff = 0;
  for (int i = 0; i < 3; i++) {
    time_t start, end; 
    time(&start);
    system(testCmd.c_str());
    time(&end);
    diff += end - start;
  }
  return diff / 3;
}

bool hasGit(std::string path) {
  return std::filesystem::exists(path + "/.git");
}

void selectMoves(std::vector<MoveInfo> movables, std::string testCmd) {
  // first try all moves
  applyMoves(movables);
  time_t bestTime = callTest(testCmd);
  std::vector<MoveInfo> bestMoves = movables;
  resetMoves(movables);
  // try binary cut
  int N = std::log(static_cast<double>(movables.size())) / std::log(2.0);
  for (int i = 0; i < N; i++) {
    std::vector<MoveInfo> newMovables;
    for (int j = 0; j < 3; j++) {
      auto rng = std::default_random_engine {};
      std::shuffle(std::begin(movables), std::end(movables), rng);
      newMovables = std::vector<MoveInfo>(movables.begin(), movables.begin() + movables.size() / 2);
      applyMoves(newMovables);
      time_t time = callTest(testCmd);
      resetMoves(newMovables);
      if (time < bestTime) { // TODO: change to significantlly smaller?
        break;
      }
    }
    movables = newMovables;
  }
  // print the results?
  // for (auto m : movables) { ... }
}

int main(int argc, const char **argv) {
  int parserArgc = argc - 1;
  auto ExpectedParser = CommonOptionsParser::create(parserArgc, argv, MyToolCategory);
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
  if (Tool.run(newFrontendActionFactory(&Finder).get())) {
    // llvm::errs() << ...
    return 1;
  }

  selectMoves(Handler.movables, argv[argc - 1]);
}