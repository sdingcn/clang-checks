#ifndef INCLUDE_VISITOR_HPP
#include "include-visitor.hpp"
#endif
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
#include <fstream>
#include <iostream>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Rewrite/Frontend/Rewriters.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <string.h>
using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

static llvm::cl::OptionCategory TestDividerCategory("my-tool options");

class FunctionMarker : public MatchFinder::MatchCallback {
    public:
    virtual void run(const MatchFinder::MatchResult &Result) override {
        TheRewriter.setSourceMgr(*Result.SourceManager, Result.Context->getLangOpts());
        const auto Ctx = Result.Context;
        const auto FuncRef = Result.Nodes.getNodeAs<FunctionDecl>("functionDecl");
        if (FuncRef && FuncRef->hasBody()) {
            auto StartLoc = FuncRef->getBody()->getSourceRange().getBegin();
            std::string cmd = "std::fstream filestr_clang_move;filestr_clang_move.open(\"" +
            std::filesystem::current_path().string() +
            "/moves.txt\", std::fstream::app | std::fstream::out);filestr_clang_move << \"" + "(" + Result.SourceManager->getFilename(FuncRef->getBeginLoc()).str() + ")\";";
            TheRewriter.InsertText(Result.SourceManager->translateLineCol(Result.SourceManager->getFileID(StartLoc), Result.SourceManager->getSpellingLineNumber(StartLoc) + 1, 1), cmd, true, true);

            std::error_code EC;
            llvm::raw_fd_ostream OutFile(
            TheRewriter.getSourceMgr().getFileEntryForID(TheRewriter.getSourceMgr().getMainFileID())->tryGetRealPathName(),
            EC, llvm::sys::fs::OF_None);

            if (EC) {
                llvm::errs() << "Could not open file for writing: " << EC.message() << "\n";
                return;
            }

            TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID()).write(OutFile);
        }
    }
    FunctionMarker(Rewriter &R) : TheRewriter(R) {};
    private:
    Rewriter &TheRewriter;
};

// class FunctionMarkerAction: public ASTFrontendAction {
// public: 
//     std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
//         CI.getDiagnostics().setSuppressAllDiagnostics(true);
//         this->TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

//         MatchFinder Finder;
//         FunctionMarker Callback(this->TheRewriter);
//         Finder.addMatcher(functionDecl(isDefinition()).bind("functionDecl"), &Callback);
//         auto AstCons = Finder.newASTConsumer();
//         return AstCons;
//     }
// private:
//     Rewriter TheRewriter;
// };

class TestDivider {
    std::vector<std::string> sample_files;
    std::string text_store;

    public: 
        TestDivider(std::string path_to_sample_dir, std::string text_store) {
            std::filesystem::path p(path_to_sample_dir);
            parse_dir(p, &(this->sample_files));
            this->text_store = text_store;
        }

        void parseFiles() {
            for (auto iter = this->sample_files.begin(); iter < this->sample_files.end(); iter++) {
                FixedCompilationDatabase CompilationsDBTestDivider(".", std::vector<std::string>());
                ClangTool Tool(CompilationsDBTestDivider, *iter);

                MatchFinder Finder;
                Rewriter TheRewriter;
                FunctionMarker Callback(TheRewriter);
                Finder.addMatcher(functionDecl(isDefinition()).bind("functionDecl"), &Callback);
                
                if (Tool.run(newFrontendActionFactory(&Finder).get()) != 0) 
                    std::cerr << "Error marking function" << std::endl;
            }
        }

    private:
        static void parse_dir(std::filesystem::path p, std::vector<std::string> *out) {
            for (const auto& entry : std::filesystem::directory_iterator(p)) {
                if (entry.is_directory()) {
                    parse_dir(entry.path(), out);
                }
                else if (strcmp(entry.path().extension().c_str(), ".cc") == 0 || strcmp(entry.path().extension().c_str(), ".cpp") == 0) {
                    out->push_back(std::filesystem::absolute(entry.path()).c_str());
                }
            }
        }
};