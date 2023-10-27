// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
// Declares llvm::cl::extrahelp.
#include "clang/AST/ASTConsumer.h"
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

std::string WriteFullSourceLocation(FullSourceLoc FullSourceLocation) {
  std::ostringstream Oss;
  Oss << FullSourceLocation.getFileEntry()->getName().str() << ":"
      << FullSourceLocation.getSpellingLineNumber() << ":"
      << FullSourceLocation.getSpellingColumnNumber();
  return Oss.str();
}

struct VariableUsage {
  FullSourceLoc FullSourceLocation;
  std::string VarName;
};

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *Context, TemplateParameterList *ParList)
    : Context(Context), ParList(ParList) {}
  bool VisitFunctionDecl(FunctionDecl *Declaration) {
    if (Declaration->isTemplateInstantiation()) {
      ArgLists.push_back(Declaration->getTemplateSpecializationArgs());
    }
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *Expression) {
    auto t = Expression->getType().getUnqualifiedType();
    if (inTemplateParameterList(t)) {
      VariableUsage VarUsage;
      VarUsage.FullSourceLocation = Context->getFullLoc(Expression->getBeginLoc());
      VarUsage.VarName = Expression->getDecl()->getNameAsString();
      VarUsages.push_back(VarUsage);
    }
    return true;
  }
  bool shouldVisitTemplateInstantiations() const { return true; }
  std::vector<const TemplateArgumentList *> ArgLists;
  std::vector<VariableUsage> VarUsages;
private:
  bool inTemplateParameterList(QualType t) {
    int n = ParList->size();
    for (int i = 0; i < n; i++) {
      if (ParList->getParam(i)->getNameAsString() == t.getAsString()) {
        return true;
      }
    }
    return false;
  }
  ASTContext *Context;
  TemplateParameterList *ParList;
};

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *Context) : Context(Context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *Declaration) {
    FullSourceLoc FullLocation =
        Context->getFullLoc(Declaration->getBeginLoc());
    if (FullLocation.isValid())
      llvm::outs() << "Found function template declaration at "
                   << WriteFullSourceLocation(FullLocation)
                   << "\n";
    TraverseFunctionTemplateVisitor Visitor(Context, Declaration->getTemplateParameters());
    Visitor.TraverseDecl(Declaration);
    llvm::outs() << "[Instantiations]\n";
    for (auto ArgList : Visitor.ArgLists) {
      llvm::outs() << "\t";
      int n = ArgList->size();
      for (int i = 0; i < n; i++) {
        llvm::outs() << (*ArgList)[i].getAsType().getAsString() << ' ';
      }
      llvm::outs() << "\n";
    }
    llvm::outs() << "[Template internal patterns]\n";
    for (auto VarUsage : Visitor.VarUsages) {
      llvm::outs() << "\t";
      llvm::outs() << VarUsage.VarName << " at "
                   << WriteFullSourceLocation(VarUsage.FullSourceLocation) << "\n";
    }
    return true;
  }

private:
  ASTContext *Context;
};

class ConceptSynthConsumer : public clang::ASTConsumer {
public:
  explicit ConceptSynthConsumer(ASTContext *Context) : Visitor(Context) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) override {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  FindTargetVisitor Visitor;
};

class ConceptSynthAction : public clang::ASTFrontendAction {
public:
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler,
                    llvm::StringRef InFile) override {
    return std::make_unique<ConceptSynthConsumer>(&Compiler.getASTContext());
  }
};

}

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
