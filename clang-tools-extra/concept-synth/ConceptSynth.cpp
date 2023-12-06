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
#include <cstdint>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <utility>
#include <sstream>
#include <tuple>
#include <functional>
#include <variant>

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

std::string WriteFullSourceLocation(FullSourceLoc fullSourceLocation) {
  std::ostringstream oss;
  oss << fullSourceLocation.getFileEntry()->getName().str() << ":"
      << fullSourceLocation.getSpellingLineNumber() << ":"
      << fullSourceLocation.getSpellingColumnNumber();
  return oss.str();
}

const TemplateTypeParmDecl *fromQualTypeToTemplateTypeParmDecl(
  QualType t, ASTContext *context, TemplateParameterList *tmpList) {
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

const FunctionTemplateDecl *fromTemplateTypeParmDeclToFunctionTemplateDecl(
  const TemplateTypeParmDecl *ttpdecl, ASTContext *context) {
  auto parents = context->getParents(*ttpdecl);
  for (auto it = parents.begin(); it != parents.end(); it++) {
    const FunctionTemplateDecl *parent = it->get<FunctionTemplateDecl>();
    if (parent) {
      return parent;
    }
  }
  return nullptr;
}

struct VariableUseExpression {
  const TemplateTypeParmDecl *ttpdecl;
  const DeclRefExpr *var;
  const Expr *expr;
  VariableUseExpression(const TemplateTypeParmDecl *t, const DeclRefExpr *v, const Expr *e)
    : ttpdecl(t), var(v), expr(e) {}
};

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *context, TemplateParameterList *tmpList)
    : context(context), tmpList(tmpList) {}

  // Get instantiations
  bool VisitFunctionDecl(FunctionDecl *declaration) {
    if (declaration->isTemplateInstantiation()) {
      argLists.push_back(declaration->getTemplateSpecializationArgs());
    }
    return true;
  }

  // Get template body usages
  bool VisitDeclRefExpr(DeclRefExpr *var) {
    auto t = var->getType();
    if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(t, context, tmpList)) {
      const DeclRefExpr &vref = *var;
      auto parents = context->getParents(vref);
      for (auto it = parents.begin(); it != parents.end(); it++) {
        if (const Expr *expr = it->get<Expr>()) {
          varUseExprs.push_back(VariableUseExpression(ttpdecl, var, expr));
        }
      }
    }
    return true;
  }

  bool shouldVisitTemplateInstantiations() const { return true; }

  std::vector<const TemplateArgumentList *> argLists;
  std::vector<VariableUseExpression> varUseExprs;

private:

  ASTContext *context;
  TemplateParameterList *tmpList;
};

// May or may not change the internal representation to QualType
struct Instantiation {
  Instantiation(const std::string &t)
    : type(t) {}

  bool operator== (const Instantiation &other) const {
    return type == other.type;
  }

  std::string str() const {
    return type;
  }

private:
  std::string type;
};

template <>
struct std::hash<Instantiation> {
  std::size_t operator() (const Instantiation &i) const {
    return std::hash<std::string>()(i.str());
  }
};

struct UnaryConstraint {
  UnaryConstraint(std::string o, int p)
    : op(std::move(o)), pos(p) {}

  bool operator== (const UnaryConstraint &other) const {
    return op == other.op && pos == other.pos;
  }

  std::string to_str() const {
    return "[UnaryCon " + op + ' ' + std::to_string(pos) + "]";
  }

  std::string op;
  int pos;
};

template <>
struct std::hash<UnaryConstraint> {
  std::size_t operator() (const UnaryConstraint &c) const {
    return std::hash<std::string>()(c.op + " " + std::to_string(c.pos));
  }
};

struct BinaryConstraint {
  BinaryConstraint(std::string o, int p)
    : op(std::move(o)), pos(p) {}

  bool operator== (const BinaryConstraint &other) const {
    return op == other.op && pos == other.pos;
  }

  std::string to_str() const {
    return "[BinaryCon " + op + ' ' + std::to_string(pos) + "]";
  }

  std::string op;
  int pos;
};

template <>
struct std::hash<BinaryConstraint> {
  std::size_t operator() (const BinaryConstraint &c) const {
    return std::hash<std::string>()(c.op + " " + std::to_string(c.pos));
  }
};

using CalleeConstraint = std::variant<QualType, const TemplateTypeParmDecl*>;

struct CallConstraint {
  CallConstraint(std::vector<CalleeConstraint> c)
    : ccs(std::move(c)) {
    std::sort(ccs.begin(), ccs.end());
  }

  bool operator== (const CallConstraint &other) const {
    return ccs == other.ccs;
  }

  std::string to_str() const {
    std::string ret = "[CallCon";
    for (const auto &cc : ccs) {
      if (std::holds_alternative<QualType>(cc)) {
        ret += ' ';
        ret += std::get<QualType>(cc).getAsString();
      } else if (std::holds_alternative<const TemplateTypeParmDecl*>(cc)) {
        ret += ' ';
        ret += std::get<const TemplateTypeParmDecl*>(cc)->getNameAsString();
      }
    }
    ret += ']';
    return ret;
  }

  std::vector<CalleeConstraint> ccs;
};

template <>
struct std::hash<CallConstraint> {
  std::size_t operator() (const CallConstraint &c) const {
    std::string s;
    for (const auto &v : c.ccs) {
      s += " ";
      if (auto qt = std::get_if<QualType>(&v)) {
        s += qt->getAsString();
      } else if (auto ttpd = std::get_if<const TemplateTypeParmDecl*>(&v)) {
        s += std::to_string(reinterpret_cast<std::uintptr_t>(*ttpd));
      }
    }
    return std::hash<std::string>()(s);
  }
};

struct MemberConstraint {
  MemberConstraint(std::string m)
    : mb(std::move(m)) {}

  bool operator== (const MemberConstraint &other) const {
    return mb == other.mb;
  }

  std::string to_str() const {
    return "[MemberCon " + mb + "]";
  }

  std::string mb;
};

template <>
struct std::hash<MemberConstraint> {
  std::size_t operator() (const MemberConstraint &c) const {
    return std::hash<std::string>()(c.mb);
  }
};

using Constraint = std::variant<UnaryConstraint, BinaryConstraint, CallConstraint, MemberConstraint>;

#if 0
namespace {
  // named requirements -> (constraint set, excluded types)
  std::unordered_map<
    std::string,
    std::pair<std::unordered_set<Constraint>, std::unordered_set<Instantiation>>
  > library = {
  };
}

// Later may add support of two or more candidates.
std::vector<std::string> infer(
  const std::unordered_set<Constraint> &constraint_set,
  const std::unordered_set<Instantiation> &instantiation_set) {
  using Candidate = std::tuple<std::string, int, int>;
  std::vector<Candidate> candidates;
  for (const auto &kv : library) {
    // Instantiations are first used to exclude candidates.
    bool ok = true;
    for (const auto &insta : instantiation_set) {
      if (kv.second.second.count(insta) > 0) {
        ok = false;
        break;
      }
    }
    if (ok) {
      // Then do comparison for constraints: coverage > uncoverage
      int same = 0;
      for (const auto &con : constraint_set) {
        if (kv.second.first.count(con) > 0) {
          same++;
        }
      }
      candidates.push_back(std::make_tuple(kv.first, same, kv.second.first.size() - same));
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &c1, const Candidate &c2) -> bool {
    if (std::get<1>(c1) > std::get<1>(c2)) {
      return true;
    } else {
      return std::get<2>(c1) > std::get<2>(c2);
    }
  });
  return std::vector<std::string>{candidates.size() > 0 ? std::get<0>(candidates[0]) : "NOTFOUND"};
}
#endif

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *declaration) {
#if 0
    FullSourceLoc fullLocation =
        context->getFullLoc(declaration->getBeginLoc());
    if (fullLocation.isValid())
      llvm::outs() << "Found function template declaration "
                   << declaration->getNameAsString()
                   << " at "
                   << WriteFullSourceLocation(fullLocation)
                   << "\n";
#endif

    TemplateParameterList *tmpList = declaration->getTemplateParameters();
    TraverseFunctionTemplateVisitor visitor(context, tmpList);
    visitor.TraverseDecl(declaration);

    for (auto varUseExpr : visitor.varUseExprs) {
      if (auto unaryOp = dyn_cast<UnaryOperator>(varUseExpr.expr)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(UnaryConstraint(unaryOp->getOpcodeStr(unaryOp->getOpcode()).str(), 0))
        );
      } else if (auto binaryOp = dyn_cast<BinaryOperator>(varUseExpr.expr)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(BinaryConstraint(
            binaryOp->getOpcodeStr(binaryOp->getOpcode()).str(),
            (binaryOp->getLHS() == varUseExpr.var ? 0 : 1)
          ))
        );
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.expr)) {
        if (auto namedCallee = dyn_cast<UnresolvedLookupExpr>(callExpr->getCallee())) {
          int position = -1;
          for (auto nodeptr = callExpr->child_begin(); nodeptr != callExpr->child_end(); nodeptr++) {
            position++;
            if ((*nodeptr) == varUseExpr.var) {
              break;
            }
          }
          std::vector<CalleeConstraint> ccs;
          for (auto declptr = namedCallee->decls_begin(); declptr != namedCallee->decls_end(); declptr++) {
            auto decl = *declptr;
            if (auto ftd = dyn_cast<FunctionTemplateDecl>(decl)) {
              auto targetTmpList = ftd->getTemplateParameters();
              auto qt = ftd->getAsFunction()->getParamDecl(position - 1)->getType();
              if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(qt, context, targetTmpList)) {
                ccs.push_back(ttpdecl);
              } else {
                ccs.push_back(qt);
              }
              for (auto specptr = ftd->spec_begin(); specptr != ftd->spec_end(); specptr++) {
                auto spec = *specptr;
                if (!(spec->isTemplateInstantiation())) {
                  auto qt = spec->getParamDecl(position - 1)->getType();
                  ccs.push_back(qt);
                }
              }
            } else if (auto fd = dyn_cast<FunctionDecl>(decl)) {
              auto qt = fd->getParamDecl(position - 1)->getType();
              ccs.push_back(qt);
            }
          }
          constraint_map[varUseExpr.ttpdecl].insert(
            Constraint(CallConstraint(ccs))
          );
        }
      } else if (auto dependentMemberExpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.expr)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(MemberConstraint(dependentMemberExpr->getMemberNameInfo().getAsString()))
        );
      }
    }

    int n = tmpList->size();
    for (int i = 0; i < n; i++) {
      auto decl = tmpList->getParam(i);
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
        for (auto argList : visitor.argLists) {
          Instantiation insta((*argList)[i].getAsType().getAsString());
          instantiation_map[ttpdecl].insert(insta);
        }
      }
    }

#if 0
    llvm::outs() << "[Inferred Named Requirements]\n";
    for (auto &kv : constraint_map) {
      llvm::outs() << '\t';
      llvm::outs() << kv.first->getNameAsString() << ": ";
      auto candidates = infer(kv.second, instantiation_map[kv.first]);
      for (auto &cand : candidates) {
        llvm::outs() << cand << ' ';
      }
      llvm::outs() << '\n';
    }

    llvm::outs() << "[Instantiations]\n";
    for (auto &kv : instantiation_map) {
      llvm::outs() << '\t';
      llvm::outs() << kv.first->getNameAsString() << ": ";
      for (auto &insta : kv.second) {
        llvm::outs() << insta.str() << ' ';
      }
      llvm::outs() << '\n';
    }
#endif

    return true;
  }

  std::unordered_map<const TemplateTypeParmDecl*, std::unordered_set<Constraint>> constraint_map;
  std::unordered_map<const TemplateTypeParmDecl*, std::unordered_set<Instantiation>> instantiation_map;

private:
  ASTContext *context;
};

class Formula {
public:
  Formula() = default;

  Formula(const Formula &f) = delete;

  Formula &operator= (const Formula &f) = delete;

  virtual ~Formula() {}

  virtual std::string getAsString() { return "(Formula)"; }

};

using AtomicConstraint = std::variant<int, QualType, UnaryConstraint, BinaryConstraint, MemberConstraint>;

class Atomic : public Formula {
public:
  Atomic() = default;

  Atomic(const Atomic &a) = delete;

  Atomic &operator= (const Atomic &a) = delete;

  ~Atomic() {}

  std::string getAsString() override {
    if (std::holds_alternative<int>(con)) {
      int i = std::get<int>(con);
      if (i == 0) {
        return "(Atom Const false)";
      } else {
        return "(Atom Const true)";
      }
    } else if (std::holds_alternative<QualType>(con)) {
      QualType t = std::get<QualType>(con);
      return "(Atom Type " + t.getAsString() + ")";
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      return "(Atom Unary " + u.op + " " + std::to_string(u.pos) + ")";
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      return "(Atom Binary " + b.op + " " + std::to_string(b.pos) + ")";
    } else if (std::holds_alternative<MemberConstraint>(con)) {
      MemberConstraint m = std::get<MemberConstraint>(con);
      return "(Atom Member " + m.mb + ")";
    } else {
      return "(Atom)";
    }
  }

  void setConstraint(const AtomicConstraint &c) {
    con = c;
  }

private:
  AtomicConstraint con;
};

class Conjunction : public Formula {
public:
  Conjunction() = default;

  Conjunction(const Conjunction &c) = delete;

  Conjunction &operator= (const Conjunction &c) = delete;

  ~Conjunction() {
    // We don't release conjuncts' memory here. Instead, we use a memory pool.
#if 0
    for (Formula *f : conjuncts) {
      delete f;
    }
#endif
  }

  std::string getAsString() override {
    std::string ret = "(and";
    for (Formula *f : conjuncts) {
      ret += " ";
      ret += f->getAsString();
    }
    ret += ")";
    return ret;
  }

  void addConjunct(Formula *f) {
    conjuncts.push_back(f);
  }

private:
  std::vector<Formula*> conjuncts;
};

class Disjunction : public Formula {
public:
  Disjunction() = default;

  Disjunction(const Disjunction &d) = delete;

  Disjunction &operator= (const Disjunction &d) = delete;

  ~Disjunction() {
    // We don't release disjuncts' memory here. Instead, we use a memory pool.
#if 0
    for (Formula *f : disjuncts) {
      delete f;
    }
#endif
  }

  std::string getAsString() override {
    std::string ret = "(or";
    for (Formula *f : disjuncts) {
      ret += " ";
      ret += f->getAsString();
    }
    ret += ")";
    return ret;
  }

  void addDisjunct(Formula *f) {
    disjuncts.push_back(f);
  }

private:
  std::vector<Formula*> disjuncts;
};

class ConceptSynthConsumer : public clang::ASTConsumer {
public:
  explicit ConceptSynthConsumer(ASTContext *context) : visitor(context) {}

  virtual void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());

#if 0
    for (const auto &kv : visitor.constraint_map) {
      llvm::outs() << kv.first->getNameAsString() << ": ";
      for (const auto &c : kv.second) {
        if (std::holds_alternative<CallConstraint>(c)) {
          llvm::outs() << " " << std::get<CallConstraint>(c).to_str();
        }
      }
      llvm::outs() << '\n';
    }
#endif

    // 0 for not visited, 1 for on stack, 2 for visited
    std::unordered_map<const TemplateTypeParmDecl*, int> status;
    std::unordered_map<const TemplateTypeParmDecl*, Formula*> results;
    std::unordered_set<Formula*> pool;

    std::function<Formula*(const TemplateTypeParmDecl*)> dfs =
    [&](const TemplateTypeParmDecl *ttpd) -> Formula* {
      if (status[ttpd] == 0) { // not visited
        status[ttpd] = 1;
        auto conj = new Conjunction();
        pool.insert(conj);
        for (const auto &c : visitor.constraint_map.at(ttpd)) {
          if (std::holds_alternative<UnaryConstraint>(c)) {
            auto unary = new Atomic();
            pool.insert(unary);
            unary->setConstraint(std::get<UnaryConstraint>(c));
            conj->addConjunct(unary);
          } else if (std::holds_alternative<BinaryConstraint>(c)) {
            auto binary = new Atomic();
            pool.insert(binary);
            binary->setConstraint(std::get<BinaryConstraint>(c));
            conj->addConjunct(binary);
          } else if (std::holds_alternative<CallConstraint>(c)) {
            auto call = new Disjunction();
            pool.insert(call);
            for (const auto &cc : std::get<CallConstraint>(c).ccs) {
              if (std::holds_alternative<QualType>(cc)) {
                auto a = new Atomic();
                pool.insert(a);
                a->setConstraint(std::get<QualType>(cc));
                call->addDisjunct(a);
              } else if (std::holds_alternative<const TemplateTypeParmDecl*>(cc)) {
                auto f = dfs(std::get<const TemplateTypeParmDecl*>(cc));
                call->addDisjunct(f);
              }
            }
            conj->addConjunct(call);
          } else if (std::holds_alternative<MemberConstraint>(c)) {
            auto member = new Atomic();
            pool.insert(member);
            member->setConstraint(std::get<MemberConstraint>(c));
            conj->addConjunct(member);
          }
        }
        status[ttpd] = 2;
        return results[ttpd] = conj;
      } else if (status[ttpd] == 1) { // on stack
        auto t = new Atomic();
        pool.insert(t);
        t->setConstraint(1);
        return t;
      } else { // visited
        return results.at(ttpd);
      }
    };

    for (const auto &kv : visitor.constraint_map) {
      auto ttpd = kv.first;
      if (status[ttpd] == 0) {
        dfs(ttpd);
      }
    }

    llvm::outs() << "[Template Body Constraints]\n";
    for (const auto &kv : results) {
      llvm::outs() << '\t';
      auto ttpd = kv.first;
      auto ftd = fromTemplateTypeParmDeclToFunctionTemplateDecl(ttpd, &context);
      if (ftd) {
        llvm::outs() << ftd->getNameAsString() << " " << ttpd->getNameAsString() << ": ";
        llvm::outs() << kv.second->getAsString() << '\n';
      }
    }

    for (auto p : pool) {
      delete p;
    }

#if 0
    for (auto &kv : results) {
      delete kv.second;
    }
#endif

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
