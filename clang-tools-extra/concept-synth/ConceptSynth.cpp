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
#include <string>
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
    auto t = expression->getType().getUnqualifiedType();
    if (inTemplateParameterList(t)) {
      const DeclRefExpr &var = *expression;
      auto parents = context->getParents(var);
      for (auto it = parents.begin(); it != parents.end(); it++) {
        const Expr *expr = it->get<Expr>();
        if (expr) {
          VariableUseExpression vue;
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
  bool inTemplateParameterList(QualType t) {
    int n = tmpList->size();
    for (int i = 0; i < n; i++) {
      if (tmpList->getParam(i)->getNameAsString() == t.getAsString()) {
        return true;
      }
    }
    return false;
  }
  ASTContext *context;
  TemplateParameterList *tmpList;
};

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *declaration) {
    FullSourceLoc fullLocation =
        context->getFullLoc(declaration->getBeginLoc());
    if (fullLocation.isValid())
      llvm::outs() << "Found function template declaration at "
                   << WriteFullSourceLocation(fullLocation)
                   << "\n";
    TraverseFunctionTemplateVisitor visitor(context, declaration->getTemplateParameters());
    visitor.TraverseDecl(declaration);
    llvm::outs() << "[Instantiations]\n";
    for (auto argList : visitor.argLists) {
      llvm::outs() << "\t";
      int n = argList->size();
      for (int i = 0; i < n; i++) {
        llvm::outs() << (*argList)[i].getAsType().getAsString() << ' ';
      }
      llvm::outs() << "\n";
    }
    llvm::outs() << "[Template Body]\n";
    for (auto varUseExpr : visitor.varUseExprs) {
      llvm::outs() << "\t";
      llvm::outs() << varUseExpr.var->getDecl()->getNameAsString() << " in "
                   << varUseExpr.expr->getStmtClassName() << "\n";
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
