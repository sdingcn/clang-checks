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

#define DEBUG llvm::errs() << "[*** DEBUG LINE " << std::to_string(__LINE__) << " ***] " << "\n"
#define PRINT(x) llvm::errs() << "[*** PRINT LINE " << std::to_string(__LINE__) << " ***] " << x << "\n"

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

// ignore all sugars, qualifiers, and references
QualType getCleanType(QualType t) {
  return t.getCanonicalType().getUnqualifiedType().getNonReferenceType();
}

const TemplateTypeParmDecl *fromQualTypeToTemplateTypeParmDecl(
  QualType t, ASTContext *context, TemplateParameterList *tmpList) {
  int n = tmpList->size();
  for (int i = 0; i < n; i++) {
    auto decl = tmpList->getParam(i);
    if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
      auto ttpt = context->getTypeDeclType(ttpdecl);
      if (context->hasSameType(ttpt, getCleanType(t))) {
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

int getNumberOfRequiredArgs(const FunctionDecl *decl) {
  int ret = 0;
  int n = decl->getNumParams();
  for (int i = 0; i < n; i++) {
    if (decl->getParamDecl(i)->hasDefaultArg()) {
      break;
    }
    ret++;
  }
  return ret;
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
  UnaryConstraint(std::string o, int p = -1)
    : op(std::move(o)), pos(p) {}

  bool operator== (const UnaryConstraint &other) const {
    return op == other.op && pos == other.pos;
  }

  std::string to_str() const {
    return "[UnaryCon " + op + ' ' + std::to_string(pos) + "]";
  }

  bool match(const UnaryConstraint &other) const {
    return op == other.op &&
           (pos == -1 || pos == other.pos);
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
  BinaryConstraint(std::string o, int p = -1, std::string t = "")
    : op(std::move(o)), pos(p), otherType(t) {}

  bool operator== (const BinaryConstraint &other) const {
    return op == other.op && pos == other.pos && otherType == other.otherType;
  }

  std::string to_str() const {
    return "[BinaryCon " + op + ' ' + std::to_string(pos) + ' ' + otherType + "]";
  }

  bool match(const BinaryConstraint &other) const {
    return op == other.op &&
           (pos == -1 || pos == other.pos) &&
           (otherType == "" || otherType == other.otherType);
  }

  std::string op;
  int pos;
  std::string otherType;
};

template <>
struct std::hash<BinaryConstraint> {
  std::size_t operator() (const BinaryConstraint &c) const {
    return std::hash<std::string>()(c.op + " " + std::to_string(c.pos) + " " + c.otherType);
  }
};

struct FunctionConstraint {
  FunctionConstraint(std::vector<std::string> p, std::string r)
    : parameterTypes(std::move(p)), returnType(std::move(r)) {}

  bool operator== (const FunctionConstraint &other) const {
    return parameterTypes == other.parameterTypes && returnType == other.returnType;
  }

  std::string to_str() const {
    std::string ret = "[FunctionCon";
    for (const std::string &s : parameterTypes) {
      ret += ' ';
      ret += s;
    }
    ret += ' ';
    ret += returnType;
    ret += ']';
    return ret;
  }

  std::vector<std::string> parameterTypes;
  std::string returnType;
};

template <>
struct std::hash<FunctionConstraint> {
  std::size_t operator() (const FunctionConstraint &c) const {
    return std::hash<std::string>()(c.to_str());
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

using Constraint = std::variant<UnaryConstraint, BinaryConstraint, FunctionConstraint, MemberConstraint, CallConstraint>;

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *declaration) {
    if (declaration->isCXXClassMember() || declaration->isCXXInstanceMember()) {
      return true;
    }
    TemplateParameterList *tmpList = declaration->getTemplateParameters();
    int len = tmpList->size();
    for (int i = 0; i < len; i++) {
      if (tmpList->getParam(i)->isParameterPack()) {
        return true;
      }
    }
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

    for (int i = 0; i < len; i++) {
      auto decl = tmpList->getParam(i);
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
        constraint_map[ttpdecl].clear();
      }
    }
    TraverseFunctionTemplateVisitor visitor(context, tmpList);
    visitor.TraverseDecl(declaration);

    for (auto varUseExpr : visitor.varUseExprs) {
      if (auto unaryOp = dyn_cast<UnaryOperator>(varUseExpr.expr)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(UnaryConstraint(unaryOp->getOpcodeStr(unaryOp->getOpcode()).str(),
            ((unaryOp->isPostfix()) ? 0 : 1)
          ))
        );
      } else if (auto binaryOp = dyn_cast<BinaryOperator>(varUseExpr.expr)) {
        std::string otherType;
        auto e = getCleanType(
          binaryOp->getLHS() == varUseExpr.var ?
          binaryOp->getRHS()->getType() :
          binaryOp->getLHS()->getType()
        );
        if (e->isDependentType()) {
          continue;
        }
        if (auto ttpd = fromQualTypeToTemplateTypeParmDecl(e, context, tmpList)) {
          otherType = ttpd->getNameAsString();
        } else {
          otherType = e.getAsString();
        }
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(BinaryConstraint(
            binaryOp->getOpcodeStr(binaryOp->getOpcode()).str(),
            (binaryOp->getLHS() == varUseExpr.var ? 0 : 1),
            otherType
          ))
        );
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.expr)) {
        if (auto tmp = dyn_cast<CXXOperatorCallExpr>(varUseExpr.expr)) {
          continue;
        }
        if (auto callable = dyn_cast<DeclRefExpr>(callExpr->getCallee())) {
          if (callable == varUseExpr.var) {
            std::vector<std::string> parameterTypes;
            for (auto arg = callExpr->arg_begin(); arg != callExpr->arg_end(); arg++) {
              parameterTypes.push_back(getCleanType((*arg)->getType()).getAsString());
            }
            std::string returnType = callExpr->getType().getAsString();
            constraint_map[varUseExpr.ttpdecl].insert(
              FunctionConstraint(parameterTypes, returnType)
            );
          }
        } else if (auto namedCallee = dyn_cast<UnresolvedLookupExpr>(callExpr->getCallee())) {
          int nArgs = callExpr->getNumArgs();
          int position = 0;
          for (auto nodeptr = callExpr->child_begin(); nodeptr != callExpr->child_end(); nodeptr++) {
            if ((*nodeptr) == varUseExpr.var) {
              break;
            }
            position++;
          }
          position--;
          std::vector<CalleeConstraint> ccs;
          bool unhandledCandidate = false;
          for (auto declptr = namedCallee->decls_begin(); declptr != namedCallee->decls_end(); declptr++) {
            auto decl = *declptr;
            if (decl->isCXXClassMember() || decl->isCXXInstanceMember()) {
              unhandledCandidate = true;
              break;
            }
            if (auto ftd = dyn_cast<FunctionTemplateDecl>(decl)) {
              TemplateParameterList *targetTmpList = ftd->getTemplateParameters();
              int targetLen = targetTmpList->size();
              bool isVariadic = false;
              for (int i = 0; i < targetLen; i++) {
                if (targetTmpList->getParam(i)->isParameterPack()) {
                  isVariadic = true;
                  break;
                }
              }
              if (isVariadic) {
                unhandledCandidate = true;
                break;
              }
              int l = getNumberOfRequiredArgs(ftd->getAsFunction());
              int r = ftd->getAsFunction()->getNumParams();
              if (nArgs >= l && nArgs <= r) {
                auto qt = ftd->getAsFunction()->getParamDecl(position)->getType();
                if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(qt, context, targetTmpList)) {
                  ccs.push_back(ttpdecl);
                } else {
                  ccs.push_back(qt);
                }
                for (auto specptr = ftd->spec_begin(); specptr != ftd->spec_end(); specptr++) {
                  auto spec = *specptr;
                  if (!(spec->isTemplateInstantiation())) {
                    auto qt = spec->getParamDecl(position)->getType();
                    ccs.push_back(qt);
                  }
                }
              }
            } else if (auto fd = dyn_cast<FunctionDecl>(decl)) {
              if (fd->isVariadic()) {
                unhandledCandidate = true;
                break;
              }
              int l = getNumberOfRequiredArgs(fd);
              int r = fd->getNumParams();
              if (nArgs >= l && nArgs <= r) {
                auto qt = fd->getParamDecl(position)->getType();
                ccs.push_back(qt);
              }
            }
          }
          // Only treat it as a constraint when every case is handled
          if (!unhandledCandidate) {
            constraint_map[varUseExpr.ttpdecl].insert(
              Constraint(CallConstraint(ccs))
            );
          }
        }
      } else if (auto dependentMemberExpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.expr)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(MemberConstraint(dependentMemberExpr->getMemberNameInfo().getAsString()))
        );
      }
    }

    for (int i = 0; i < len; i++) {
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

using AtomicConstraint = std::variant<QualType, UnaryConstraint, BinaryConstraint, FunctionConstraint, MemberConstraint>;

class Formula {
public:
  Formula() = default;

  Formula(const Formula &f) = delete;

  Formula &operator= (const Formula &f) = delete;

  virtual ~Formula() {}

  virtual std::string getAsString() const {
    return "(Formula)";
  }

  virtual int literalStatus() const {
    return -1;
  }

  virtual bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const {
    return false;
  }

  virtual std::string printConcept(const std::string &templateTypeParm) const {
    return "";
  }
};

class Literal : public Formula {
public:
  Literal(bool v) : value(v) {}

  Literal(const Literal &l) = delete;

  Literal &operator= (const Literal &l) = delete;

  ~Literal() override {}

  std::string getAsString() const override {
    if (value) {
      return "(Literal true)";
    } else {
      return "(Literal false)";
    }
  }

  int literalStatus() const override {
    if (value) {
      return 1;
    } else {
      return 0;
    }
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    return value;
  }

  std::string printConcept(const std::string &templateTypeParm) const override {
    return value ? "true" : "false";
  }

  bool value;
};

class Atomic : public Formula {
public:
  Atomic(const AtomicConstraint &c) : con(c) {}

  Atomic(const Atomic &a) = delete;

  Atomic &operator= (const Atomic &a) = delete;

  ~Atomic() override {}

  std::string getAsString() const override {
    if (std::holds_alternative<QualType>(con)) {
      QualType t = std::get<QualType>(con);
      return "(Atom Type " + t.getAsString() + ")";
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      return "(Atom Unary " + u.to_str() + ")";
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      return "(Atom Binary " + b.to_str() + ")";
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      return "(Atom Function " + f.to_str() + ")";
    } else {
      MemberConstraint m = std::get<MemberConstraint>(con);
      return "(Atom Member " + m.to_str() + ")";
    }
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    if (std::holds_alternative<QualType>(con)) {
      return false;
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      return has_constraint(u);
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      return has_constraint(b);
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      return has_constraint(f);
    } else {
      MemberConstraint m = std::get<MemberConstraint>(con);
      return has_constraint(m);
    }
  }

  std::string printConcept(const std::string &templateTypeParm) const override {
    if (std::holds_alternative<QualType>(con)) {
      QualType t = std::get<QualType>(con);
      return "std::convertible_to<" + templateTypeParm + ", " + t.getAsString() + ">";
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      std::string e = ((u.pos == 0) ? ("x" + u.op) : (u.op + "x"));
      return "requires (" + templateTypeParm + " x) { " + e + "; }";
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      std::string e = ((b.pos == 0) ? ("x" + b.op + "y") : ("y" + b.op + "x"));
      return "requires (" + templateTypeParm + " x, " + b.otherType + " y) { " + e + "; }";
    } else {
      // TODO
      return "true";
    }
  }

  AtomicConstraint con;
};

class Conjunction : public Formula {
public:
  Conjunction() = default;

  Conjunction(const Conjunction &c) = delete;

  Conjunction &operator= (const Conjunction &c) = delete;

  ~Conjunction() override {
    // We don't release conjuncts' memory here. Instead, we use a memory pool.
  }

  std::string getAsString() const override {
    std::string ret = "(and";
    for (Formula *f : conjuncts) {
      ret += " ";
      ret += f->getAsString();
    }
    ret += ")";
    return ret;
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    bool ret = true;
    for (auto f : conjuncts) {
      ret = ret && (f->evaluate(has_constraint));
    }
    return ret;
  }

  std::string printConcept(const std::string &templateTypeParm) const override {
    std::string ret;
    for (auto f : conjuncts) {
      if (ret != "") {
        ret += " && ";
      }
      ret += f->printConcept(templateTypeParm);
    }
    return "(" + ret + ")";
  }

  void addConjunct(Formula *f) {
    conjuncts.push_back(f);
  }

  std::vector<Formula*> conjuncts;
};

class Disjunction : public Formula {
public:
  Disjunction() = default;

  Disjunction(const Disjunction &d) = delete;

  Disjunction &operator= (const Disjunction &d) = delete;

  ~Disjunction() override {
    // We don't release disjuncts' memory here. Instead, we use a memory pool.
  }

  std::string getAsString() const override {
    std::string ret = "(or";
    for (Formula *f : disjuncts) {
      ret += " ";
      ret += f->getAsString();
    }
    ret += ")";
    return ret;
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    bool ret = false;
    for (auto f : disjuncts) {
      ret = ret || (f->evaluate(has_constraint));
    }
    return ret;
  }

  std::string printConcept(const std::string &templateTypeParm) const override {
    std::string ret;
    for (auto f : disjuncts) {
      if (ret != "") {
        ret += " || ";
      }
      ret += f->printConcept(templateTypeParm);
    }
    return "(" + ret + ")";
  }

  void addDisjunct(Formula *f) {
    disjuncts.push_back(f);
  }

  std::vector<Formula*> disjuncts;
};

class Pool {
public:
  Pool() = default;

  Pool(const Pool &p) = delete;

  Pool &operator= (const Pool &p) = delete;

  ~Pool() {
    for (auto ptr : pointers) {
      delete ptr;
    }
  }

  template <typename T, typename... Args>
  T *poolNew(Args&&... args) {
    auto ptr = new T(std::forward<Args>(args)...);
    pointers.push_back(ptr);
    return ptr;
  }

private:
  std::vector<Formula*> pointers;
};


namespace namedrequirements {
  template <typename T>
  bool matchConstraintType(const AtomicConstraint &c) {
    return std::holds_alternative<T>(c);
  }
  template <typename T>
  bool matchConstraintValue(const AtomicConstraint &c, const T &pattern) {
    if (std::holds_alternative<T>(c)) {
      return pattern.match(std::get<T>(c));
    } else {
      return false;
    }
  }
  bool iteratorHasConstraint(const AtomicConstraint &c) {
    if (
      matchConstraintType<UnaryConstraint>(c) ||
      matchConstraintType<BinaryConstraint>(c)
    ) {
      if (
        matchConstraintValue(c, UnaryConstraint("*", 1)) ||
        matchConstraintValue(c, UnaryConstraint("++", 1)) ||
        matchConstraintValue(c, BinaryConstraint("="))
      ) {
        return true;
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
  bool inputIteratorHasConstraint(const AtomicConstraint &c) {
    if (iteratorHasConstraint(c)) {
      return true;
    } else if (
      matchConstraintType<UnaryConstraint>(c) ||
      matchConstraintType<BinaryConstraint>(c)
    ) {
      if (
        matchConstraintValue(c, BinaryConstraint("==")) ||
        matchConstraintValue(c, BinaryConstraint("!=")) ||
        matchConstraintValue(c, UnaryConstraint("++", 0))
      ) {
        return true;
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
  bool outputIteratorHasConstraint(const AtomicConstraint &c) {
    if (iteratorHasConstraint(c)) {
      return true;
    } else if (
      matchConstraintType<UnaryConstraint>(c) ||
      matchConstraintType<BinaryConstraint>(c)
    ) {
      if (
        matchConstraintValue(c, UnaryConstraint("++", 0))
      ) {
        return true;
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
  bool forwardIteratorHasConstraint(const AtomicConstraint &c) {
    return inputIteratorHasConstraint(c);
  }
  bool bidirectionalIteratorHasConstraint(const AtomicConstraint &c) {
    if (forwardIteratorHasConstraint(c)) {
      return true;
    } else if (
      matchConstraintType<UnaryConstraint>(c) ||
      matchConstraintType<BinaryConstraint>(c)
    ) {
      if (
        matchConstraintValue(c, UnaryConstraint("--"))
      ) {
        return true;
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
  bool randomAccessIteratorHasConstraint(const AtomicConstraint &c) {
    if (bidirectionalIteratorHasConstraint(c)) {
      return true;
    } else if (
      matchConstraintType<UnaryConstraint>(c) ||
      matchConstraintType<BinaryConstraint>(c)
    ) {
      if (
        matchConstraintValue(c, BinaryConstraint("+")) ||
        matchConstraintValue(c, BinaryConstraint("-")) ||
        matchConstraintValue(c, BinaryConstraint("<")) ||
        matchConstraintValue(c, BinaryConstraint(">")) ||
        matchConstraintValue(c, BinaryConstraint("<=")) ||
        matchConstraintValue(c, BinaryConstraint(">="))
      ) {
        return true;
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
  // named requirements ->
  // (has_constraint, has_instantiation)
  std::vector<std::pair<
    std::string,
    std::pair<std::function<bool(const AtomicConstraint&)>, std::function<bool(const Instantiation&)>>
  >> iteratorRequirements = {
    {"Iterator",
      {iteratorHasConstraint, [](const Instantiation &i){ return true; }}},
    {"InputIterator",
      {inputIteratorHasConstraint, [](const Instantiation &i){ return true; }}},
    {"OutputIterator",
      {outputIteratorHasConstraint, [](const Instantiation &i){ return true; }}},
    {"ForwardIterator",
      {forwardIteratorHasConstraint, [](const Instantiation &i){ return true; }}},
    {"BidirectionalIterator",
      {bidirectionalIteratorHasConstraint, [](const Instantiation &i){ return true; }}},
    {"RandomAccessIterator",
      {randomAccessIteratorHasConstraint, [](const Instantiation &i){ return true; }}}
  };
}

// Later may add support of two or more named requirements.
std::vector<std::string> infer(
  const Formula *formula,
  const std::unordered_set<Instantiation> &instantiation_set) {
  std::vector<std::string> requirements;
  for (const auto &[name, predicates] : namedrequirements::iteratorRequirements) {
    const auto &[constraint_predicate, instantiation_predicate] = predicates;
    bool ok1 = formula->evaluate(constraint_predicate);
    bool ok2 = true;
    for (const auto &i : instantiation_set) {
      if (!instantiation_predicate(i)) {
        ok2 = false;
        break;
      }
    }
    if (ok1 && ok2) {
      requirements.push_back(name);
      break;
    }
  }
  return requirements;
}

class ConceptSynthConsumer : public clang::ASTConsumer {
public:
  explicit ConceptSynthConsumer(ASTContext *context) : visitor(context) {}

  virtual void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());

    // 0 for not visited, 1 for on stack, 2 for visited
    std::unordered_map<const TemplateTypeParmDecl*, int> status;
    std::unordered_map<const TemplateTypeParmDecl*, Formula*> results;
    Pool pool;

    std::function<Formula*(const TemplateTypeParmDecl*)> dfs =
    [&](const TemplateTypeParmDecl *ttpd) -> Formula* {
      if (status[ttpd] == 0) { // not visited
        status[ttpd] = 1;
        auto conj = pool.poolNew<Conjunction>();
        bool triviallyFalse = false;
        for (const auto &c : visitor.constraint_map.at(ttpd)) {
          if (std::holds_alternative<UnaryConstraint>(c)) {
            auto u = pool.poolNew<Atomic>(std::get<UnaryConstraint>(c));
            conj->addConjunct(u);
          } else if (std::holds_alternative<BinaryConstraint>(c)) {
            auto b = pool.poolNew<Atomic>(std::get<BinaryConstraint>(c));
            conj->addConjunct(b);
          } else if (std::holds_alternative<MemberConstraint>(c)) {
            auto m = pool.poolNew<Atomic>(std::get<MemberConstraint>(c));
            conj->addConjunct(m);
          } else if (std::holds_alternative<FunctionConstraint>(c)) {
            auto f = pool.poolNew<Atomic>(std::get<FunctionConstraint>(c));
            conj->addConjunct(f);
          } else if (std::holds_alternative<CallConstraint>(c)) {
            auto disj = pool.poolNew<Disjunction>();
            bool triviallyTrue = false;
            for (const auto &cc : std::get<CallConstraint>(c).ccs) {
              if (std::holds_alternative<QualType>(cc)) {
                auto a = pool.poolNew<Atomic>(std::get<QualType>(cc));
                disj->addDisjunct(a);
              } else if (std::holds_alternative<const TemplateTypeParmDecl*>(cc)) {
                if (auto f = dfs(std::get<const TemplateTypeParmDecl*>(cc))) {
                  if (f->literalStatus() != -1) {
                    if (f->literalStatus() == 1) {
                      triviallyTrue = true;
                      break;
                    } else {
                      continue;
                    }
                  } else {
                    disj->addDisjunct(f);
                  }
                } else { // recursive dependency
                  triviallyTrue = true;
                  break;
                }
              }
            }
            if (triviallyTrue) {
              continue;
            } else if (disj->disjuncts.size() == 0) {
              triviallyFalse = true;
              break;
            } else if (disj->disjuncts.size() == 1) {
              conj->addConjunct(disj->disjuncts[0]);
            } else {
              conj->addConjunct(disj);
            }
          }
        }
        status[ttpd] = 2;
        if (triviallyFalse) {
          return results[ttpd] = pool.poolNew<Literal>(false);
        } else if (conj->conjuncts.size() == 0) {
          return results[ttpd] = pool.poolNew<Literal>(true);
        } else if (conj->conjuncts.size() == 1) {
          return results[ttpd] = conj->conjuncts[0];
        } else {
          return results[ttpd] = conj;
        }
      } else if (status[ttpd] == 1) { // on stack
        return nullptr;
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

    for (const auto &kv : results) {
      auto ttpd = kv.first;
      auto f = kv.second;
      auto ftd = fromTemplateTypeParmDeclToFunctionTemplateDecl(ttpd, &context);
      if (ftd) {
        llvm::outs() << "[" << ftd->getNameAsString() << ", " << ttpd->getNameAsString() << "]\n";
        llvm::outs() << "\tRaw constraint: " << f->getAsString() << '\n';
        llvm::outs() << "\tPrinted code: " << f->printConcept(ttpd->getNameAsString()) << '\n';
        llvm::outs() << "\tInferred constraint:";
        const auto &inferred = infer(f, visitor.instantiation_map[ttpd]);
        for (const auto &con : inferred) {
          llvm::outs() << " " << con;
        }
        llvm::outs() << "\n";
      }
    }

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
