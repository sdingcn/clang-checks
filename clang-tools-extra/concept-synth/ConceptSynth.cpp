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

std::string stringFormat(const std::string &pattern, const std::vector<std::string> &elements) {
  std::string result;
  int i = 0;
  int s = elements.size();
  for (char c : pattern) {
    if (c == '#') {
      if (i >= s) {
        result += '#';
      } else {
        result += elements[i];
      }
      i++;
    } else {
      result += c;
    }
  }
  return result;
}

std::string getFullSourceLocationAsString(std::variant<const Decl*, const Stmt*> node, ASTContext *ctx) {
  SourceLocation sl = 
    std::holds_alternative<const Decl*>(node) ?
    std::get<const Decl*>(node)->getBeginLoc() :
    std::get<const Stmt*>(node)->getBeginLoc();
  FullSourceLoc fl = ctx->getFullLoc(sl);
  if (fl.isValid()) {
    std::ostringstream oss;
    oss << fl.getFileEntry()->getName().str() << ":"
        << fl.getSpellingLineNumber() << ":"
        << fl.getSpellingColumnNumber();
    return oss.str();
  } else {
    return "";
  }
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

const Stmt *getFirstStmtParent(const Stmt *s, ASTContext *ctx) {
  const Stmt &ref = *s;
  auto parents = ctx->getParents(ref);
  for (auto it = parents.begin(); it != parents.end(); it++) {
    if (const Stmt *ps = it->get<Stmt>()) {
      return ps;
    }
  }
  return nullptr;
}

std::unordered_map<std::string, std::function<bool(const Stmt*, const Stmt*)>> simpleInferences {
  {"bool", [](const Stmt *s, const Stmt *p) -> bool {
    if (auto ifStmt = dyn_cast<IfStmt>(p)) {
      if (ifStmt->getCond() == s) {
        return true;
      }
    } else if (auto whileStmt = dyn_cast<WhileStmt>(p)) {
      if (whileStmt->getCond() == s) {
        return true;
      }
    } else if (auto forStmt = dyn_cast<ForStmt>(p)) {
      if (forStmt->getCond() == s) {
        return true;
      }
    }
    return false;
  }}
};

struct VariableUseStmt {
  const TemplateTypeParmDecl *ttpdecl;
  QualType type;
  const DeclRefExpr *var;
  const Stmt *stmt;
  VariableUseStmt(const TemplateTypeParmDecl *tt, QualType t, const DeclRefExpr *v, const Stmt *s)
    : ttpdecl(tt), type(t), var(v), stmt(s) {}
};

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *c, FunctionTemplateDecl *f)
    : context(c), functionTemplateDeclaration(f) {}

  // Get instantiations
  bool VisitFunctionDecl(FunctionDecl *declaration) {
    if (declaration->isTemplateInstantiation()) {
      argLists.push_back(declaration->getTemplateSpecializationArgs());
    }
    return true;
  }

  // Get template body usages
  bool VisitDeclRefExpr(DeclRefExpr *var) {
    // ignore compound template types like T*, std::vector<T>, etc.
    if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(
      var->getType(),
      context,
      functionTemplateDeclaration->getTemplateParameters()
    )) {
      if (auto ps = getFirstStmtParent(var, context)) {
        varUseStmts.push_back(VariableUseStmt(ttpdecl, var->getType(), var, ps));
      }
    }
    return true;
  }

  bool shouldVisitTemplateInstantiations() const {
    return true;
  }

  std::vector<const TemplateArgumentList *> argLists;
  std::vector<VariableUseStmt> varUseStmts;

private:

  ASTContext *context;
  FunctionTemplateDecl *functionTemplateDeclaration;
};

struct Instantiation {
  Instantiation(QualType t)
    : type(t) {}

  bool operator== (const Instantiation &other) const {
    return type == other.type;
  }

  std::string to_str() const {
    return type.getAsString();
  }

private:
  QualType type;
};

template <>
struct std::hash<Instantiation> {
  std::size_t operator() (const Instantiation &i) const {
    return std::hash<std::string>()(i.to_str());
  }
};

// TODO: for constraints, consider cv-qualifiers and references

using TypeBox = std::variant<std::string, QualType>;

std::string typeBoxToString(const TypeBox &t) {
  if (std::holds_alternative<std::string>(t)) {
    return std::get<std::string>(t);
  } else {
    return std::get<QualType>(t).getAsString();
  }
}

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
    return std::hash<std::string>()(c.to_str());
  }
};

struct BinaryConstraint {
  BinaryConstraint(std::string o, int p = -1, TypeBox t = "")
    : op(std::move(o)), pos(p), otherType(t) {}

  bool operator== (const BinaryConstraint &other) const {
    return op == other.op && pos == other.pos && otherType == other.otherType;
  }

  std::string to_str() const {
    return "[BinaryCon " + op + ' ' + std::to_string(pos) + ' ' + typeBoxToString(otherType) + "]";
  }

  bool match(const BinaryConstraint &other) const {
    return op == other.op &&
            (pos == -1 || pos == other.pos) &&
            (
              (std::holds_alternative<std::string>(otherType) && (std::get<std::string>(otherType) == ""))
              || otherType == other.otherType
            );
  }

  std::string op;
  int pos;
  TypeBox otherType;
};

template <>
struct std::hash<BinaryConstraint> {
  std::size_t operator() (const BinaryConstraint &c) const {
    return std::hash<std::string>()(c.to_str());
  }
};

struct FunctionConstraint {
  FunctionConstraint(std::vector<TypeBox> p, TypeBox r)
    : parameterTypes(std::move(p)), returnType(r) {}

  bool operator== (const FunctionConstraint &other) const {
    return parameterTypes == other.parameterTypes && returnType == other.returnType;
  }

  std::string to_str() const {
    std::string ret = "[FunctionCon";
    for (const auto &t : parameterTypes) {
      ret += ' ';
      ret += typeBoxToString(t);
    }
    ret += ' ';
    ret += typeBoxToString(returnType);
    ret += ']';
    return ret;
  }

  std::vector<TypeBox> parameterTypes;
  TypeBox returnType;
};

template <>
struct std::hash<FunctionConstraint> {
  std::size_t operator() (const FunctionConstraint &c) const {
    return std::hash<std::string>()(c.to_str());
  }
};

struct MemberConstraint {
  MemberConstraint(std::string m, bool i, std::vector<TypeBox> p, TypeBox r)
    : mb(std::move(m)), isFun(i), parameterTypes(std::move(p)), returnType(r) {}

  bool operator== (const MemberConstraint &other) const {
    return mb == other.mb && isFun == other.isFun
           && parameterTypes == other.parameterTypes && returnType == other.returnType;
  }

  std::string to_str() const {
    std::string ret = "[MemberCon";
    ret += ' ';
    ret += std::to_string(isFun ? 1 : 0);
    for (const auto &t : parameterTypes) {
      ret += ' ';
      ret += typeBoxToString(t);
    }
    ret += ' ';
    ret += typeBoxToString(returnType);
    ret += ']';
    return ret;
  }

  std::string mb;
  bool isFun;
  std::vector<TypeBox> parameterTypes;
  TypeBox returnType;
};

template <>
struct std::hash<MemberConstraint> {
  std::size_t operator() (const MemberConstraint &c) const {
    return std::hash<std::string>()(c.to_str());
  }
};

using CalleeConstraint = std::variant<TypeBox, const TemplateTypeParmDecl*>;

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
      if (std::holds_alternative<TypeBox>(cc)) {
        ret += ' ';
        ret += typeBoxToString(std::get<TypeBox>(cc));
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
    return std::hash<std::string>()(c.to_str());
  }
};

using Constraint = std::variant<UnaryConstraint, BinaryConstraint, FunctionConstraint, MemberConstraint, CallConstraint>;

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *declaration) {
    // exclude class / instance members
    if (declaration->isCXXClassMember() || declaration->isCXXInstanceMember()) {
      return true;
    }
    TemplateParameterList *tmpList = declaration->getTemplateParameters();
    // exclude variadic templates
    for (auto p = tmpList->begin(); p != tmpList->end(); p++) {
      if ((*p)->isParameterPack()) {
        return true;
      }
    }
    // initialize constraint_map
    for (auto p = tmpList->begin(); p != tmpList->end(); p++) {
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(*p)) {
        constraint_map[ttpdecl].clear();
      }
    }
    // get (1) template-typed variable usages (2) instantiations
    TraverseFunctionTemplateVisitor visitor(context, declaration);
    visitor.TraverseDecl(declaration);
    // handle template-typed variable usages
    for (auto varUseExpr : visitor.varUseStmts) {
      if (auto unaryOp = dyn_cast<UnaryOperator>(varUseExpr.stmt)) {
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(UnaryConstraint(unaryOp->getOpcodeStr(unaryOp->getOpcode()).str(),
            ((unaryOp->isPostfix()) ? 0 : 1)
          ))
        );
      } else if (auto binaryOp = dyn_cast<BinaryOperator>(varUseExpr.stmt)) {
        auto otherType = getCleanType(
          binaryOp->getLHS() == varUseExpr.var ?
          binaryOp->getRHS()->getType() :
          binaryOp->getLHS()->getType()
        );
        constraint_map[varUseExpr.ttpdecl].insert(
          Constraint(BinaryConstraint(
            binaryOp->getOpcodeStr(binaryOp->getOpcode()).str(),
            (binaryOp->getLHS() == varUseExpr.var ? 0 : 1),
            otherType
          ))
        );
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.stmt)) {
        // ignore overloaded operators
        if (auto tmp = dyn_cast<CXXOperatorCallExpr>(varUseExpr.stmt)) {
          continue;
        }
        // var used as function
        if (auto callable = dyn_cast<DeclRefExpr>(callExpr->getCallee())) {
          if (callable == varUseExpr.var) {
            std::vector<TypeBox> parameterTypes;
            for (auto arg = callExpr->arg_begin(); arg != callExpr->arg_end(); arg++) {
              parameterTypes.push_back(getCleanType((*arg)->getType()));
            }
            TypeBox returnType = "";
            for (const auto &[type, checker] : simpleInferences) {
              auto parent = getFirstStmtParent(callExpr, context);
              if (parent && checker(callExpr, parent)) {
                returnType = type;
                break;
              }
            }
            constraint_map[varUseExpr.ttpdecl].insert(
              FunctionConstraint(std::move(parameterTypes), returnType)
            );
          }
        // var used as argument
        } else if (auto namedCallee = dyn_cast<UnresolvedLookupExpr>(callExpr->getCallee())) {
          // var used as the pos-th argument
          int pos = 0;
          for (auto nodeptr = callExpr->arg_begin(); nodeptr != callExpr->arg_end(); nodeptr++) {
            if ((*nodeptr) == varUseExpr.var) {
              break;
            }
            pos++;
          }
          // constraints imposed by overloading candidates
          std::vector<CalleeConstraint> ccs;
          bool hasUnhandledCandidate = false;
          int nArgs = callExpr->getNumArgs();
          for (auto declit = namedCallee->decls_begin(); declit != namedCallee->decls_end(); declit++) {
            auto decl = *declit;
            // ignore class / instance members
            if (decl->isCXXClassMember() || decl->isCXXInstanceMember()) {
              hasUnhandledCandidate = true;
              break;
            }
            // candidate is a function template
            if (auto ftd = dyn_cast<FunctionTemplateDecl>(decl)) {
              TemplateParameterList *tpl = ftd->getTemplateParameters();
              bool isVariadic = false;
              for (auto p = tpl->begin(); p != tpl->end(); p++) {
                if ((*p)->isParameterPack()) {
                  isVariadic = true;
                  break;
                }
              }
              // ignore variadic templates
              if (isVariadic) {
                hasUnhandledCandidate = true;
                break;
              }
              int l = getNumberOfRequiredArgs(ftd->getAsFunction());
              int r = ftd->getAsFunction()->getNumParams();
              if (nArgs >= l && nArgs <= r) {
                auto qt = ftd->getAsFunction()->getParamDecl(pos)->getType();
                if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(qt, context, tpl)) {
                  ccs.push_back(ttpdecl);
                } else {
                  // ignore dependent types depending on other templates' type parameters
                  if (qt->isDependentType()) {
                    hasUnhandledCandidate = true;
                    break;
                  } else {
                    ccs.push_back(qt);
                  }
                }
                // specializations
                for (auto specit = ftd->spec_begin(); specit != ftd->spec_end(); specit++) {
                  auto spec = *specit;
                  if (!(spec->isTemplateInstantiation())) {
                    auto qt = spec->getParamDecl(pos)->getType();
                    ccs.push_back(qt);
                  }
                }
              } else {
                hasUnhandledCandidate = true;
                break;
              }
            // candidate is a function
            } else if (auto fd = dyn_cast<FunctionDecl>(decl)) {
              // ignore variadic functions
              if (fd->isVariadic()) {
                hasUnhandledCandidate = true;
                break;
              }
              int l = getNumberOfRequiredArgs(fd);
              int r = fd->getNumParams();
              if (nArgs >= l && nArgs <= r) {
                auto qt = fd->getParamDecl(pos)->getType();
                ccs.push_back(qt);
              }
            // not sure what the candidate is
            } else {
              hasUnhandledCandidate = true;
              break;
            }
          }
          // only treat it as a constraint when every case is handled
          if (!hasUnhandledCandidate) {
            constraint_map[varUseExpr.ttpdecl].insert(
              Constraint(CallConstraint(ccs))
            );
          }
        }
      } else if (auto mexpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.stmt)) {
        // ignore member accesses via ->
        if (mexpr->isArrow()) {
          continue;
        }
        auto parent = getFirstStmtParent(mexpr, context);
        auto possibleMemberCall = parent ? dyn_cast<CallExpr>(parent) : nullptr;
        // this is a member function
        if (possibleMemberCall && possibleMemberCall->getCallee() == mexpr) {
          std::vector<TypeBox> parameterTypes;
          for (auto arg = possibleMemberCall->arg_begin(); arg != possibleMemberCall->arg_end(); arg++) {
            parameterTypes.push_back(getCleanType((*arg)->getType()));
          }
          TypeBox returnType = "";
          for (const auto &[type, checker] : simpleInferences) {
            auto parent = getFirstStmtParent(possibleMemberCall, context);
            if (parent && checker(possibleMemberCall, parent)) {
              returnType = type;
              break;
            }
          }
          constraint_map[varUseExpr.ttpdecl].insert(
            Constraint(MemberConstraint(
              mexpr->getMemberNameInfo().getAsString(),
              true,
              std::move(parameterTypes),
              returnType
            ))
          );
        // this is a member variable
        } else {
          TypeBox returnType = "";
          for (const auto &[type, checker] : simpleInferences) {
            auto parent = getFirstStmtParent(mexpr, context);
            if (parent && checker(mexpr, parent)) {
              returnType = type;
              break;
            }
          }
          constraint_map[varUseExpr.ttpdecl].insert(
            Constraint(MemberConstraint(
              mexpr->getMemberNameInfo().getAsString(),
              false,
              {},
              returnType
            ))
          );
        }
      }
    }

    // handle instantiations
    int len = tmpList->size();
    for (int i = 0; i < len; i++) {
      auto decl = tmpList->getParam(i);
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
        for (auto argList : visitor.argLists) {
          Instantiation insta((*argList)[i].getAsType());
          instantiation_map[ttpdecl].insert(insta);
        }
      }
    }
  
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

private:
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
    return has_constraint(con);
  }

  std::string printConcept(const std::string &templateTypeParm) const override {
    if (std::holds_alternative<QualType>(con)) {
      QualType t = std::get<QualType>(con);
      return stringFormat("std::convertible_to<#, #>", {templateTypeParm, t.getAsString()});
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      std::string expr = ((u.pos == 0) ? ("x" + u.op) : (u.op + "x"));
      return stringFormat("requires (# x) { #; }", {templateTypeParm, expr});
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      std::string expr = ((b.pos == 0) ? ("x" + b.op + "y") : ("y" + b.op + "x"));
      return stringFormat("requires (# x, # y) { #; }", {templateTypeParm, typeBoxToString(b.otherType), expr});
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      int np = f.parameterTypes.size();
      std::string pattern = "requires (# f, ";
      for (int i = 0; i < np; i++) {
        pattern += ("# x" + std::to_string(i) + ", ");
      }
      pattern.pop_back();
      pattern.pop_back();
      pattern += ") { #; }";
      std::string call = "f(";
      for (int i = 0; i < np; i++) {
        call += ("x" + std::to_string(i) + ", ");
      }
      if (call.back() == ' ') {
        call.pop_back();
        call.pop_back();
      }
      call += ")";
      if (typeBoxToString(f.returnType) != "") {
        call = "{" + call + "} -> " + "std::convertible_to<" + typeBoxToString(f.returnType) + ">";
      }
      std::vector<std::string> elements;
      elements.push_back(templateTypeParm);
      for (const auto &p : f.parameterTypes) {
        elements.push_back(typeBoxToString(p));
      }
      elements.push_back(call);
      return stringFormat(pattern, elements);
    } else if (std::holds_alternative<MemberConstraint>(con)) {
      MemberConstraint m = std::get<MemberConstraint>(con);
      int np = m.parameterTypes.size();
      std::string pattern = "requires (# o, ";
      for (int i = 0; i < np; i++) {
        pattern += ("# x" + std::to_string(i) + ", ");
      }
      pattern.pop_back();
      pattern.pop_back();
      pattern += ") { #; }";
      std::string access = "o." + m.mb;
      if (m.isFun) {
        access += "(";
        for (int i = 0; i < np; i++) {
          access += ("x" + std::to_string(i) + ", ");
        }
        if (access.back() == ' ') {
          access.pop_back();
          access.pop_back();
        }
        access += ")";
      }
      if (typeBoxToString(m.returnType) != "") {
        access = "{" + access + "} -> " + "std::convertible_to<" + typeBoxToString(m.returnType) + ">";
      }
      std::vector<std::string> elements;
      elements.push_back(templateTypeParm);
      for (const auto &p : m.parameterTypes) {
        elements.push_back(typeBoxToString(p));
      }
      elements.push_back(access);
      return stringFormat(pattern, elements);
    } else {
      return "true";
    }
  }

private:
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
    std::string ret = "";
    for (auto f : conjuncts) {
      auto conj = f->printConcept(templateTypeParm);
      if (conj != "true") {
        if (ret != "") {
          ret += " && ";
        }
        ret += conj;
      }
    }
    if (ret == "") {
      return "true";
    } else {
      return "(" + ret + ")";
    }
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
    std::string ret = "";
    for (auto f : disjuncts) {
      auto disj = f->printConcept(templateTypeParm);
      if (disj != "false") {
        if (ret != "") {
          ret += " || ";
        }
        ret += disj;
      }
    }
    if (ret == "") {
      return "false";
    } else {
      return "(" + ret + ")";
    }
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
              if (std::holds_alternative<TypeBox>(cc)) {
                auto a = pool.poolNew<Atomic>(std::get<QualType>(std::get<TypeBox>(cc)));
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
