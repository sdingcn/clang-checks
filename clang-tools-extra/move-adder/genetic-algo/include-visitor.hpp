#include <iostream>
#include <string>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>

using namespace clang;
using namespace clang::tooling;

class IncludeFinderVisitor : public RecursiveASTVisitor<IncludeFinderVisitor> {
public:
    IncludeFinderVisitor(Rewriter &R) : TheRewriter(R), FoundInclude(false) {}

    bool VisitDecl(Decl *D) {
        if (FoundInclude) {
            return true; // Stop if the include is already found
        }

        if (auto *ID = dyn_cast<InclusionDirective>(D)) {
            std::string IncludeName = ID->getFileName().str();
            if (IncludeName == "fstream") {
                FoundInclude = true;
            }
        }

        return true;
    }

    bool shouldInsertInclude() const {
        return !FoundInclude;
    }

private:
    Rewriter &TheRewriter;
    bool FoundInclude;
};

class IncludeFinderConsumer : public ASTConsumer {
public:
    IncludeFinderConsumer(Rewriter &R) : Visitor(R), TheRewriter(R) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());

        if (Visitor.shouldInsertInclude()) {
            const FileID MainFileID = TheRewriter.getSourceMgr().getMainFileID();
            SourceLocation InsertLoc = TheRewriter.getSourceMgr().getLocForStartOfFile(MainFileID);
            TheRewriter.InsertText(InsertLoc, "#include <vector>\n", true, true);
        }
    }

private:
    IncludeFinderVisitor Visitor;
    Rewriter &TheRewriter;
};

class IncludeFinderAction : public ASTFrontendAction {
public:
    IncludeFinderAction() {}

    void EndSourceFileAction() override {
        TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID()).write(llvm::outs());
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<IncludeFinderConsumer>(TheRewriter);
    }

private:
    Rewriter TheRewriter;
};