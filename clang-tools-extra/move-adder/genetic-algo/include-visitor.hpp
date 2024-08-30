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

class IncludeFinderCallback : public PPCallbacks {
public:
    IncludeFinderCallback(Rewriter &R) : TheRewriter(R), FoundInclude(false) {}

    void InclusionDirective(SourceLocation HashLoc,
                                  const Token &IncludeTok, StringRef FileName,
                                  bool IsAngled, CharSourceRange FilenameRange,
                                  OptionalFileEntryRef File,
                                  StringRef SearchPath, StringRef RelativePath,
                                  const Module *SuggestedModule,
                                  bool ModuleImported,
                                  SrcMgr::CharacteristicKind FileType) override {
        if (FoundInclude) {
            return;
        }

        if (FileName == "fstream") {
            FoundInclude = true;
            llvm::outs() << "#include <fstream> is found.\n";
        }
    }

    bool shouldInsertInclude() const {
        return !FoundInclude;
    }

private:
    Rewriter &TheRewriter;
    bool FoundInclude;
};

class IncludeFinderAction : public PreprocessorFrontendAction {
public:
    IncludeFinderAction() {}

    void EndSourceFileAction() override {
        TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID()).write(llvm::outs());
    }

    void ExecuteAction() override {
        // Set up the rewriter
        TheRewriter.setSourceMgr(getCompilerInstance().getSourceManager(), getCompilerInstance().getLangOpts());
        // Add the preprocessor callback
        getCompilerInstance().getPreprocessor().addPPCallbacks(std::make_unique<IncludeFinderCallback>(TheRewriter));
    }

private:
    Rewriter TheRewriter;
};