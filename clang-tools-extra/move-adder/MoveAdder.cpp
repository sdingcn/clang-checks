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
#include <iostream>
#include <sstream>
#include <cstring>
#include <nlohmann/json.hpp>

#define PRINT(x) llvm::errs() << "[LINE " << std::to_string(__LINE__) << "] " << (x) << "\n"

#ifdef _WIN32
#include <windows.h>

bool isFileEditable(const std::string& filePath) {
    DWORD attributes = GetFileAttributes(filePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "Error getting file attributes: " << GetLastError() << std::endl;
        return false; // File does not exist or other error
    }

    // Check if the file is not read-only
    return !(attributes & FILE_ATTRIBUTE_READONLY);
}

#else // Assume Linux or other Unix-like system
#include <sys/stat.h>

bool isFileEditable(const std::string& filePath) {
    struct stat fileInfo;

    if (stat(filePath.c_str(), &fileInfo) != 0) {
        std::cerr << "Error getting file status." << std::endl;
        return false; // File does not exist or other error
    }

    // Check if the file is writable by the owner
    return (fileInfo.st_mode & S_IWUSR) != 0; // Check owner's write permission
}

#endif

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::tidy::utils;
using json = nlohmann::json;


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
    if (Loc.getFileEntry() != nullptr) {
      auto FName = Loc.getFileEntry()->tryGetRealPathName().str();
      auto FPos = std::make_pair(Loc.getSpellingLineNumber(), Loc.getSpellingColumnNumber());
      return std::make_pair(FName, FPos);
    } else {
      return std::make_pair("N/A", std::make_pair(-1, -1));
    }
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
  std::string toString() const {
      std::ostringstream out;
      out << File << ":"
          << "(" << Loc.first << ", " << Loc.second << "):"
          << varName;
      return out.str();
  }
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
      Ctx->getSourceManager().getDiagnostics().setSuppressAllDiagnostics(true);
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


void applyMoves(const std::vector<MoveInfo> &movables) {
  // Maps file names/line numbers to move locations
  std::map<std::string,
    std::map<int, std::vector<std::pair<int, int>>>
  > M;
  for (auto &m : movables) {
    if (isFileEditable(m.File)) {
      // cppreference: [] inserts value_type(key, T()) if the key does not exist.
      M[m.File][m.Loc.first].push_back(std::make_pair(m.Loc.second, m.varName.size()));
    }
  }

  std::cout << "size: " << M.size() << std::endl;
  std::cerr << "started sort" << std::endl;
  for (auto &p1 : M) {
    for (auto &p2 : p1.second) {
      std::sort(p2.second.begin(), p2.second.end(),
        [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
          return a.first > b.first;
        }
      );
    }
  }

  std::cerr << "ended sort" << std::endl;
  for (auto &p1 : M) {
    std::string fname = p1.first;
    std::cerr << fname << std::endl;
    std::fstream fs(fname, std::ios::in | std::ios::out);
    if (!fs) {
      continue;
    } 
    std::vector<std::string> lines;
    std::string line;
    while (getline(fs, line)) {
      if (fs.fail()) {
        std::cerr << "Failed to read file" << std::endl;
        continue;
      }
      lines.push_back(line); // currently not getting file
    }
    std::cerr << "finished instream" << std::endl;
    std::cerr << fname << std::endl;
    for (auto &p2 : p1.second) {
      int lineno = p2.first - 1;
      for (auto &p3 : p2.second) {
        int column = p3.first - 1;
        int varLen = p3.second;
        int endPos = column + varLen;
        std::cerr << "line num: " << lineno << std::endl;
        std::cerr << "endPos: " << endPos << std::endl;
        std::cerr << lines.size() << std::endl;
        if (lineno != -1) {
          lines[lineno].insert(endPos, ")");
          lines[lineno].insert(column, "std::move(");
          std::cout << lines[lineno] << std::endl;
        }
      }
    }

    std::cerr << "finished building output" << std::endl;
    fs << "#include <utility> \n";
    for (auto &l : lines) {
      fs << l << '\n';
    }

    std::cerr << "finished applying output" << std::endl;
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

void resetMoves(const std::string &buildPath) {
  auto projectPath = std::filesystem::path(buildPath) / std::filesystem::path("..");
  auto gitCmd = "git -C " + projectPath.string() + " restore .";
  std::system(gitCmd.c_str());
}

time_t callTest(std::string testCmd, std::string buildCmd) {
  std::system(buildCmd.c_str());
  time_t diff = 0;
  for (int i = 0; i < 3; i++) {
    time_t start, end; 
    time(&start);
    std::system(testCmd.c_str());
    time(&end);
    diff += end - start;
  }
  return diff / 3;
}

bool hasGit(std::string path) {
  return std::filesystem::exists(path + "/.git");
}

std::vector<MoveInfo> selectMoves(
    std::vector<MoveInfo> movables, std::string testCmd, std::string buildPath, std::string buildCmd
) {
  // first try the original time
  std::cerr << "started" << std::endl;
  // applyMoves(movables);
  time_t originalTime = callTest(testCmd, buildCmd);
  // std::vector<MoveInfo> bestMoves = movables;
  // resetMoves(buildPath);
  // try binary cut
  int N = std::log(static_cast<double>(movables.size())) / std::log(2.0);
  for (int i = 0; i < N; i++) {
    std::vector<MoveInfo> newMovables;
    for (int j = 0; j < 3; j++) {
      auto rng = std::default_random_engine {};
      std::shuffle(std::begin(movables), std::end(movables), rng);
      newMovables = std::vector<MoveInfo>(movables.begin(), movables.begin() + movables.size() / 2);
      applyMoves(newMovables);
      time_t time = callTest(testCmd, buildCmd);
      resetMoves(buildPath);
      if (time < originalTime) { // TODO: change to significantlly smaller?
        break;
      }
    }
    movables = newMovables;
  }
  return movables;
}

#if 0
std::pair<std::vector<std::string>, std::string> listFilesInDirectory(const std::string& directoryPath) {
    std::vector<std::string> filePaths;
    std::string compile_commands_json = "";

      // Iterate through the directory
      for (const auto& entry : std::filesystem::recursive_directory_iterator(directoryPath)) {
          // Check if the entry is a regular file
          if (entry.is_regular_file() && (entry.path().extension().string() == ".cc" || entry.path().extension().string() == ".cpp" || entry.path().extension().string() == ".h" || entry.path().extension().string() == ".hpp")) {
              // Add the file path to the vector
              filePaths.push_back(entry.path().string());
          }
          bool tmp = entry.path().filename().string() == "compile_commands.json";
          if (tmp) {
            compile_commands_json = entry.path().string();
          }
      }

    return std::make_pair(filePaths, compile_commands_json);
}
#endif

#if 0
std::string getCommand(std::string val) {
  int i = 0;
  if (val.size() == 0) {
    std::cerr << "Invalid include path" << std::endl;
    exit(1);
  }
  while (val.at(i) != ' ') {
    if (i == val.size()) {
      std::cerr << "Invalid include path in compile_commands.json, please regeneratae file" << std::endl;
      exit(1);
    }
    i++;
  }
  return val.substr(i, val.size());
}
#endif

std::string make_absolute(std::string file, std::string directory) {
  std::filesystem::path filepath(file);
  std::filesystem::path dirpath(directory);
  if (filepath.is_absolute()) {
    return filepath;
  } 

  return std::filesystem::absolute(dirpath/filepath);
} 

std::vector<std::string> read_include_paths(std::string compile_commands_json) {
  std::ifstream f(compile_commands_json);
  std::vector<std::string> out;

  if (!f) {
    out.push_back("invalid file");
    return out;
  }
  json data = json::parse(f);
  for (auto iter = data.items().begin(); iter != data.items().end(); iter++) {
    std::string dir = (iter).value()["directory"];
    std::string fpath = (iter).value()["file"];
    out.push_back(make_absolute(fpath, dir));
  }
  return out;
}

#if 0
std::pair<char**, int> concat_ptr(std::pair<char**, int> ptr1, std::pair<char**, int> ptr2) {
  char** retVal = new char*[ptr1.second + ptr2.second];
  for (int i = 0; i < ptr1.second; i++) {
    retVal[i] = ptr1.first[i];
  }

  for (int i = ptr1.second; i < ptr2.second + ptr1.second; i++) {
    retVal[i] = ptr2.first[i - ptr1.second];
  }

  return std::make_pair(retVal, ptr1.second + ptr2.second);
}
#endif

int main(int argc, const char **argv) {
  int parserArgc = argc - 1;

  if (argc != 5) {
    std::cerr << "usage: move-adder <path to compile-commands.json> <test-command> <path-to-build-dir> <build-command>" << std::endl;
    return 1;
  }

  std::vector<MoveInfo> movables;
  auto include_paths = read_include_paths(argv[1]);
  if (include_paths[0] == "invalid file") {
    std::cerr << "arg1 must be a filepath" << std::endl;
    return 1;
  }
  // include_paths.clear();
  // include_paths.push_back("/nethome/vmodgil3/opencv/modules/gapi/src/backends/cpu/gcpustereo.cpp");
  long move_err = 0;
  long move_good = 0;
  for (int i = 0; i < include_paths.size(); i++) {
    std::string file = include_paths[i];
    std::cerr << "file: "<< file << " at " << i << "\n";
    int argc = 4;
    char** argv_custom = new char*[argc];

    std::string ma = argv[0];
    argv_custom[0] = new char[ma.length() + 1];
    strcpy(argv_custom[0], ma.c_str());

    std::string dash = "-p";
    argv_custom[1] = new char[dash.length() + 1];
    strcpy(argv_custom[1], dash.c_str());

    std::string build_path = argv[1];
    argv_custom[2] = new char[build_path.length() + 1];
    strcpy(argv_custom[2], build_path.c_str());

    argv_custom[3] = new char[file.length() + 1];
    strcpy(argv_custom[3], file.c_str());


    const char** new_argv = (const char**) argv_custom;
    auto ExpectedParser = CommonOptionsParser::create(argc, new_argv, MyToolCategory);
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
      std::cerr << "error in file" << std::endl;
      move_err++;
    } else {
      move_good++;
    }

    movables.insert(movables.end(), Handler.movables.begin(), Handler.movables.end());
    std::cerr << "Good file count: " << move_good << std::endl;
    std::cerr << "Bad file count: " << move_err << std::endl;

    // std::cerr << Handler.movables.size() << " new moves found: " << file << std::endl; 
    std::cerr << movables.size() << " total moves found" << std::endl; 
  }
  auto moves = selectMoves(std::move(movables), argv[2], argv[3], argv[4]);
  for (auto &m : moves) {
      std::cout << m.toString() << std::endl;
  }
}
