// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
// Declares llvm::cl::extrahelp.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/CommandLine.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <utility>
#include <sstream>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...\n");

// The core classes
namespace {

std::string WriteFullSourceLocation(FullSourceLoc fullSourceLocation) {
  std::ostringstream oss;
  oss << fullSourceLocation.getFileEntry()->getName().str() << ":"
      << fullSourceLocation.getSpellingLineNumber() << ":"
      << fullSourceLocation.getSpellingColumnNumber();
  return oss.str();
}

struct VariableUseExpression {
  const TemplateTypeParmDecl *ttpdecl;
  const DeclRefExpr *var;
  const Expr *expr;
};

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *context, TemplateParameterList *tmpList)
    : context(context), tmpList(tmpList) {}
  bool VisitFunctionDecl(FunctionDecl *declaration) {
    if (declaration->isTemplateInstantiation()) {
      argLists.push_back(declaration->getTemplateSpecializationArgs());
    }
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *expression) {
    auto t = expression->getType();
    if (auto ttpdecl = getTemplateTypeParmDecl(t)) {
      const DeclRefExpr &var = *expression;
      auto parents = context->getParents(var);
      for (auto it = parents.begin(); it != parents.end(); it++) {
        const Expr *expr = it->get<Expr>();
        if (expr) {
          VariableUseExpression vue;
          vue.ttpdecl = ttpdecl;
          vue.var = expression;
          vue.expr = expr;
          varUseExprs.push_back(vue);
        }
      }
    }
    return true;
  }
  bool shouldVisitTemplateInstantiations() const { return true; }
  std::vector<const TemplateArgumentList *> argLists;
  std::vector<VariableUseExpression> varUseExprs;
private:
  TemplateTypeParmDecl *getTemplateTypeParmDecl(QualType t) {
    int n = tmpList->size();
    for (int i = 0; i < n; i++) {
      auto decl = tmpList->getParam(i);
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
        auto ttpt = context->getTypeDeclType(ttpdecl);
        if (context->hasSameType(ttpt, t)) {
          return ttpdecl;
        }
      }
    }
    return nullptr;
  }
  ASTContext *context;
  TemplateParameterList *tmpList;
};

struct Constraint {
  std::string category;
  std::string name;
  int position;
  Constraint(const std::string &c, const std::string &n, int p)
    : category(c), name(n), position(p) {}
  std::string str() {
    return "(" + category + ", " + name + ", " + std::to_string(position) + ")";
  }
};

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *declaration) {

    FullSourceLoc fullLocation =
        context->getFullLoc(declaration->getBeginLoc());
    if (fullLocation.isValid())
      llvm::outs() << "Found function template declaration "
                   << declaration->getNameAsString()
                   << " at "
                   << WriteFullSourceLocation(fullLocation)
                   << "\n";

    TraverseFunctionTemplateVisitor visitor(context, declaration->getTemplateParameters());
    visitor.TraverseDecl(declaration);

    std::vector<std::vector<std::string>> instantiations;
    for (auto argList : visitor.argLists) {
      std::vector<std::string> insta;
      int n = argList->size();
      for (int i = 0; i < n; i++) {
        insta.push_back((*argList)[i].getAsType().getAsString());
      }
      instantiations.push_back(std::move(insta));
    }

    std::unordered_map<std::string, std::vector<Constraint>> constraint_map;
    std::unordered_map<std::string, std::unordered_set<std::string>> dedup;
    for (auto varUseExpr : visitor.varUseExprs) {
      std::string type = varUseExpr.ttpdecl->getNameAsString();
      std::string category;
      std::string name;
      int position = -1;
      if (auto unaryOp = dyn_cast<UnaryOperator>(varUseExpr.expr)) {
        category = "UnaryOperator";
        name = unaryOp->getOpcodeStr(unaryOp->getOpcode());
        position = 0;
      } else if (auto binaryOp = dyn_cast<BinaryOperator>(varUseExpr.expr)) {
        category = "BinaryOperator";
        name = binaryOp->getOpcodeStr(binaryOp->getOpcode());
        if (binaryOp->getLHS() == varUseExpr.var) {
          position = 0;
        } else {
          position = 1;
        }
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.expr)) {
        if (auto namedCallee = dyn_cast<UnresolvedLookupExpr>(callExpr->getCallee())) {
          category = "CallExpr";
          name = namedCallee->getName().getAsString();
          int i = 0;
          for (auto node = namedCallee->child_begin(); node != namedCallee->child_end(); node++) {
            if ((*node) == varUseExpr.var) {
              position = i;
              break;
            }
            i++;
          }
        } else {
          continue;
        }
      } else if (auto dependentMemberExpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.expr)) {
        category = "CXXDependentScopeMemberExpr";
        name = dependentMemberExpr->getMemberNameInfo().getAsString();
        position = 0;
      } else {
        continue;
      }
      Constraint con(category, name, position);
      if (dedup[type].count(con.str()) == 0) {
        constraint_map[type].push_back(con);
        dedup[type].insert(con.str());
      }
    }

    llvm::outs() << "[Instantiations]\n";
    for (auto &insta : instantiations) {
      llvm::outs() << '\t';
      for (auto &type : insta) {
        llvm::outs() << type << "  ";
      }
      llvm::outs() << '\n';
    }

    llvm::outs() << "[Template Body]\n";
    for (auto &kv : constraint_map) {
      llvm::outs() << '\t';
      llvm::outs() << kv.first << ": ";
      for (auto &con : kv.second) {
        llvm::outs() << con.str() << ' ';
      }
      llvm::outs() << '\n';
    }

    return true;
  }

private:
  ASTContext *context;
};

class ConceptSynthConsumer : public clang::ASTConsumer {
public:
  explicit ConceptSynthConsumer(ASTContext *context) : visitor(context) {}

  virtual void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  FindTargetVisitor visitor;
};

class ConceptSynthAction : public clang::ASTFrontendAction {
public:
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    return std::make_unique<ConceptSynthConsumer>(&compiler.getASTContext());
  }
};

} // namespace

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
  return Tool.run(newFrontendActionFactory<ConceptSynthAction>().get());
}
