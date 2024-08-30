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

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

static llvm::cl::OptionCategory MyToolCategory("my-tool options");

class FunctionMarker : public MatchFinder::MatchCallback {
    virtual void run(const MatchFinder::MatchResult &Result) {
        const auto Ctx = Result.Context;
        const auto FuncRef = Result.Nodes.getNodeAs<FunctionDecl>("functionDecl");
        Rewriter TheRewriter;
        if (FuncRef && FuncRef->hasBody()) {
            auto StartLoc = FuncRef->getBody()->getSourceRange().getBegin().getLocWithOffset(1);
            std::string cmd = "std::fstream filestr_clang_move;filestr_clang_move.open(\"" +
            std::filesystem::current_path().string() +
            "/moves.txt\", std::fstream::app | std::fstream::out);filestr_clang_move << \"" + "(" + Result.SourceManager->getFilename(FuncRef->getBeginLoc()).str() + ")\";";
            TheRewriter.InsertText(StartLoc, cmd);
        }
    }
};

class TestDivider {
    std::vector<std::string> sample_files;
    std::string text_store;

    public: 
        TestDivider(std::string path_to_sample_dir, std::string text_store) {
            std::filesystem::path p(path_to_sample_dir);
            parse_dir(p, this->sample_files);
            this->text_store = text_store;
        }

        void parseFiles() {
            auto FunctionMatcher = functionDecl();
            FixedCompilationDatabase Compilations(".", std::vector<std::string>());
            FunctionMarker Marker;

            MatchFinder Finder;

            Finder.addMatcher(functionDecl().bind("functionDecl"), &Marker);

            FixedCompilationDatabase Compilations(".", std::vector<std::string>());

            ClangTool Tool(Compilations, this->sample_files);

            if (Tool.run(newFrontendActionFactory(&Finder).get()) != 0) 
                std::cerr << "Error marking function" << std::endl;
        }

    private:
        static void parse_dir(std::filesystem::path p, std::vector<std::string> out) {
            std::filesystem::directory_iterator start(p);
            std::filesystem::directory_iterator end;

            for (auto iter  = start; iter != end; iter++) {
                if (std::filesystem::is_directory(iter->status())) {
                    parse_dir(iter->path(), out);
                } else {
                    if (iter->path().extension() == "cpp" || iter->path().extension() == "cc") {
                        out.push_back(iter->path().c_str());
                    }
                }
            }
        }
};