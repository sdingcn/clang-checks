#ifndef INCLUDE_VISITOR_HPP
#define INCLUDE_VISITOR_HPP
#include <iostream>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Rewrite/Core/Rewriter.h>
#endif

using namespace clang;

class IncludeInserter : public PPCallbacks {
    bool fstreamIncluded = false;
    Rewriter &TheRewriter;
    
public:
    IncludeInserter(Rewriter &R) : TheRewriter(R) {}

    void InclusionDirective(SourceLocation HashLoc,
                                const Token &IncludeTok, StringRef FileName,
                                bool IsAngled, CharSourceRange FilenameRange,
                                OptionalFileEntryRef File,
                                StringRef SearchPath, StringRef RelativePath,
                                const Module *SuggestedModule,
                                bool ModuleImported,
                                SrcMgr::CharacteristicKind FileType) override {
        // Check if <fstream> is included
        if (FileName == "fstream") {
            fstreamIncluded = true;
        }
    }

    bool isFstreamIncluded() const {
        return fstreamIncluded;
    }

    void insertFstreamIncludeIfNeeded(SourceManager &SM) {
        if (!fstreamIncluded) {
            // Insert the include at the beginning of the file
            SourceLocation SL = SM.getLocForStartOfFile(SM.getMainFileID());
            TheRewriter.InsertText(SL, "#include <fstream>\n", true, true);
        }
    }
};

class FileStreamVisitor : public RecursiveASTVisitor<FileStreamVisitor> {
    ASTContext *Context;
    bool usesFileStream = false;

public:
    explicit FileStreamVisitor(ASTContext *Context)
        : Context(Context) {}

    bool VisitCXXRecordDecl(CXXRecordDecl *Declaration) {
        // Check if the class is std::ifstream or std::ofstream
        if (Declaration->getQualifiedNameAsString() == "std::ifstream" ||
            Declaration->getQualifiedNameAsString() == "std::ofstream") {
            usesFileStream = true;
        }
        return true;
    }

    bool usesFileStreamInCode() const {
        return usesFileStream;
    }
};

class FileStreamASTConsumer : public ASTConsumer {
    FileStreamVisitor Visitor;
    IncludeInserter &Inserter;

public:
    FileStreamASTConsumer(ASTContext *Context, IncludeInserter &Inserter)
        : Visitor(Context), Inserter(Inserter) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
        if (Visitor.usesFileStreamInCode()) {
            Inserter.insertFstreamIncludeIfNeeded(Context.getSourceManager());
        }
    }
};

class FileStreamFrontendAction : public ASTFrontendAction {
    Rewriter TheRewriter;
    
public:
    void EndSourceFileAction() override {
        TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID())
            .write(llvm::outs());
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        auto Inserter = std::make_unique<IncludeInserter>(TheRewriter);
        CI.getPreprocessor().addPPCallbacks(std::move(Inserter));
        return std::make_unique<FileStreamASTConsumer>(&CI.getASTContext(), *Inserter);
    }
};