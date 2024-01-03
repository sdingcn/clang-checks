// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
// Declares llvm::cl::extrahelp.
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <string>
#include <utility>
#include <sstream>
#include <tuple>
#include <functional>
#include <variant>
#include <optional>

#define DEBUG llvm::errs() << "[*** DEBUG LINE " << std::to_string(__LINE__) << " ***] " << "\n"
#define PRINT(x) llvm::errs() << "[*** PRINT LINE " << std::to_string(__LINE__) << " ***] " << x << "\n"
#define CLASS_STRING(x) ("(" + std::to_string(__LINE__) + " " + (x) + ")")

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

/******************************************************
 * helper functions
 ******************************************************/

std::string interpolate(const std::vector<std::string> &strs) {
  std::string result;
  for (const std::string &s : strs) {
    if (result == "") {
      result += " ";
    }
    result += s;
  }
  return result;
}

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

std::string replaceAll(const std::string &s, char p, const std::string &w) {
  std::string ret;
  for (char c : s) {
    if (c == p) {
      ret += w;
    } else {
      ret += c;
    }
  }
  return ret;
}

std::string getFullSourceLocationAsString(std::variant<const Decl*, const Stmt*> node, ASTContext *ctx) {
  SourceLocation sl = 
    std::holds_alternative<const Decl*>(node) ?
    std::get<const Decl*>(node)->getBeginLoc() :
    std::get<const Stmt*>(node)->getBeginLoc();
  FullSourceLoc fl = ctx->getFullLoc(sl);
  if (fl.isValid()) {
    std::ostringstream oss;
    std::string fpath;
    if (auto f = fl.getFileEntry()) {
      fpath = f->getName().str();
    }
    unsigned line = fl.getSpellingLineNumber();
    unsigned column = fl.getSpellingColumnNumber();
    oss << fpath << ":" << line << ":" << column;
    return oss.str();
  } else {
    return "";
  }
}

bool isVariadicFunctionTemplate(const FunctionTemplateDecl *ftdecl) {
  TemplateParameterList *tplist = ftdecl->getTemplateParameters();
  for (auto p = tplist->begin(); p != tplist->end(); p++) {
    if ((*p)->isParameterPack()) {
      return true;
    }
  }
  return false;
}

// ignore all sugars, qualifiers, and references
QualType getCleanType(QualType t) {
  return t.getCanonicalType().getUnqualifiedType().getNonReferenceType();
}

const TemplateTypeParmDecl *fromQualTypeToTemplateTypeParmDecl(
  QualType t, const TemplateParameterList *tplist, ASTContext *context) {
  int n = tplist->size();
  for (int i = 0; i < n; i++) {
    auto decl = tplist->getParam(i);
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

bool isValidNumberOfArgs(const FunctionDecl *fdecl, int nArgs) {
  int l = getNumberOfRequiredArgs(fdecl);
  int r = fdecl->getNumParams();
  return nArgs >= l && nArgs <= r;
}

bool isValidNumberOfArgs(const FunctionTemplateDecl *ftdecl, int nArgs) {
  int l = getNumberOfRequiredArgs(ftdecl->getAsFunction());
  int r = ftdecl->getAsFunction()->getNumParams();
  return nArgs >= l && nArgs <= r;
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

std::optional<std::string> getTSTName(const TemplateSpecializationType *tst) {
  auto tdecl = tst->getTemplateName().getAsTemplateDecl();
  if (tdecl) {
    return tdecl->getQualifiedNameAsString();
  } else {
    return std::nullopt;
  }
}

const Expr *getEnableIfBoolExpr(QualType t) {
  auto ct = t.getCanonicalType(); // desugar std::enable_if_t
  if (auto dnt = ct->getAs<DependentNameType>()) {
    auto qu = dnt->getQualifier();
    auto id = dnt->getIdentifier();
    if (qu && id && id->getName() == "type") {
       if (auto st = qu->getAsType()) { // container struct type
        if (auto tst = st->getAs<TemplateSpecializationType>()) {
          auto name = getTSTName(tst);
          if (name.has_value() && name.value() == "std::enable_if") {
            auto args = tst->template_arguments();
            if (args.size() > 0 && args[0].getKind() == TemplateArgument::ArgKind::Expression) {
              return args[0].getAsExpr();
            }
          }
        }
      }
    }
  }
  return nullptr;
}

bool isMoveOrForward(const Stmt *s) {
  if (auto callExpr = dyn_cast<CallExpr>(s)) {
    if (callExpr->getNumArgs() == 1) {
      if (auto callee = dyn_cast<UnresolvedLookupExpr>(callExpr->getCallee())) {
        auto name = callee->getName().getAsString();
        if (auto qu = callee->getQualifier()) {
          if (auto ns = qu->getAsNamespace()) {
            if (ns->getNameAsString() == "std" && (name == "move" || name == "forward")) {
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

/******************************************************
 * constraint classes
 ******************************************************/

struct TypeTraitExpression {
  TypeTraitExpression(bool n, std::string p)
    : neg(n), predicate(std::move(p)) {}
  
  bool operator== (const TypeTraitExpression &other) const {
    return neg == other.neg && predicate == other.predicate;
  }

  std::string toStr() const {
    std::string content = neg ? "! " : " ";
    content += predicate;
    return CLASS_STRING(content);
  }

  bool neg;
  std::string predicate;
};

struct TypeTraitConstraint {
  TypeTraitConstraint(const TypeTraitExpression &e, const TemplateTypeParmDecl *t)
    : expr(e), ttpdecl(t) {}

  bool operator== (const TypeTraitConstraint &other) const {
    return expr == other.expr && ttpdecl == other.ttpdecl;
  }

  std::string toStr() const {
    std::string content = expr.toStr();
    content += std::to_string(reinterpret_cast<uintptr_t>(ttpdecl));
    return CLASS_STRING("TypeTraitConstraint " + content);
  }

  bool match(const TypeTraitConstraint &other) const {
    return (*this) == other;
  }

  TypeTraitExpression expr;
  const TemplateTypeParmDecl *ttpdecl;
};

template <>
struct std::hash<TypeTraitConstraint> {
  std::size_t operator() (const TypeTraitConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

std::unordered_set<std::string> SupportedTraits {
  "std::is_void",
  "std::is_null_pointer",
  "std::is_integral",
  "std::is_floating_point",
  "std::is_array",
  "std::is_enum",
  "std::is_union",
  "std::is_class",
  "std::is_function",
  "std::is_pointer"
};

std::optional<TypeTraitConstraint> tryTraitConstraint(
  const Expr *e,
  const TemplateParameterList *tplist,
  ASTContext *ctx
) {
  if (auto e0 = dyn_cast<ImplicitCastExpr>(e)) {
    e = e0->getSubExpr();
  }
  bool neg = false;
  if (auto e0 = dyn_cast<UnaryOperator>(e)) {
    if (e0->getOpcode() == UnaryOperator::Opcode::UO_LNot) {
      e = e0->getSubExpr();
      neg = true;
    }
  }
  if (auto d = dyn_cast<DependentScopeDeclRefExpr>(e)) {
    auto qu = d->getQualifier();
    auto id = d->getDeclName();
    if (qu && id && id.getAsString() == "value") {
      if (auto st = qu->getAsType()) { // container struct type
        if (auto tst = st->getAs<TemplateSpecializationType>()) {
          auto name = getTSTName(tst);
          if (name.has_value() && SupportedTraits.count(name.value()) > 0) {
            auto args = tst->template_arguments();
            if (args.size() > 0 && args[0].getKind() == TemplateArgument::ArgKind::Type) {
              if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(args[0].getAsType(), tplist, ctx)) {
                return TypeTraitConstraint(TypeTraitExpression(neg, name.value()), ttpdecl);
              }
            }
          }
        }
      }
    }
  }
  return std::nullopt;
}

// currently support non-dependent types and template type parameter types
using SupportedType = std::variant<QualType, const TemplateTypeParmDecl*>;

std::string supportedTypeToString(const SupportedType &s) {
  if (std::holds_alternative<QualType>(s)) {
    return std::get<QualType>(s).getAsString();
  } else {
    return std::get<const TemplateTypeParmDecl*>(s)->getNameAsString();
  }
}

std::optional<SupportedType> trySupportedType(
  QualType t,
  const TemplateParameterList *tplist,
  ASTContext *ctx
) {
  if (t->isDependentType()) {
    auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(t, tplist, ctx);
    if (ttpdecl) {
      return ttpdecl;
    } else {
      return std::nullopt;
    }
  } else {
    return t;
  }
}

bool matchOptionalSupportedType(
  const std::optional<SupportedType> &t1,
  const std::optional<SupportedType> &t2
) {
  if (t1.has_value() && t2.has_value()) {
    return t1.value() == t2.value();
  } else {
    return true;
  }
}

std::string optionalSupportedTypeToString(const std::optional<SupportedType> &t) {
  if (t.has_value()) {
    return supportedTypeToString(t.value());
  } else {
    return "_";
  }
}

struct UnaryConstraint {
  UnaryConstraint(std::string o, std::optional<SupportedType> s = std::nullopt, int p = -1)
    : op(std::move(o)), selfType(s), pos(p) {}

  bool operator== (const UnaryConstraint &other) const {
    return op == other.op && selfType == other.selfType && pos == other.pos;
  }

  std::string toStr() const {
    std::string content = interpolate({op, optionalSupportedTypeToString(selfType), std::to_string(pos)});
    return CLASS_STRING("UnaryConstraint " + content);
  }

  bool match(const UnaryConstraint &other) const {
    return op == other.op &&
           matchOptionalSupportedType(selfType, other.selfType) &&
           (pos == -1 || other.pos == -1 || pos == other.pos);
  }

  std::string op;
  std::optional<SupportedType> selfType;
  int pos;
};

template <>
struct std::hash<UnaryConstraint> {
  std::size_t operator() (const UnaryConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct DependentExpression {
  DependentExpression(const TemplateTypeParmDecl *d, std::string o, int p)
    : ttpdecl(d), op(o), pos(p) {}
  
  bool operator== (const DependentExpression &other) const {
    return ttpdecl == other.ttpdecl && op == other.op && pos == other.pos;
  }

  std::string toStr() const {
    std::string content = interpolate({
      std::to_string(reinterpret_cast<uintptr_t>(ttpdecl)),
      op,
      std::to_string(pos)
    });
    return CLASS_STRING(content);
  }

  const TemplateTypeParmDecl *ttpdecl;
  std::string op;
  int pos;
};

using Argum = std::variant<QualType, DependentExpression>;

std::string argumToString(const Argum &a) {
  if (std::holds_alternative<QualType>(a)) {
    return std::get<QualType>(a).getAsString();
  } else {
    return std::get<DependentExpression>(a).toStr();
  }
}

std::optional<Argum> tryArgum(
  const Expr *expr,
  const TemplateParameterList *tplist,
  ASTContext *ctx 
) {
  auto t = expr->getType();
  if (t->isDependentType()) {
    if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(t, tplist, ctx)) {
      return DependentExpression(ttpdecl, "", -1);
    } else if (auto uo = dyn_cast<UnaryOperator>(expr)) {
      auto subexpr = uo->getSubExpr();
      auto st = subexpr->getType();
      if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(st, tplist, ctx)) {
        return DependentExpression(
          ttpdecl,
          uo->getOpcodeStr(uo->getOpcode()).str(),
          ((uo->isPostfix()) ? 0 : 1)
        );
      } else {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  } else {
    return t;
  }
}

struct BinaryConstraint {
  BinaryConstraint(std::string o, std::optional<SupportedType> s = std::nullopt,
                   int p = -1, std::optional<Argum> e = std::nullopt)
    : op(std::move(o)), selfType(s), pos(p), otherExpr(e) {
    // normalization
    if (e.has_value() && std::holds_alternative<DependentExpression>(e.value())) {
      const DependentExpression &de = std::get<DependentExpression>(e.value());
      if (de.op == "") { // de is just a variable
        if (s.has_value() && std::holds_alternative<const TemplateTypeParmDecl*>(s.value())) {
          if (de.ttpdecl == std::get<const TemplateTypeParmDecl*>(s.value())) {
            // lhs and rhs are symmetric
            pos = 0;
          }
        }
      }
    }
  }

  bool operator== (const BinaryConstraint &other) const {
    return op == other.op && selfType == other.selfType && pos == other.pos && otherExpr == other.otherExpr;
  }

  std::string toStr() const {
    std::string content = interpolate({
      op,
      optionalSupportedTypeToString(selfType),
      std::to_string(pos),
      (otherExpr.has_value() ? argumToString(otherExpr.value()) : "_")
    });
    return CLASS_STRING("BinaryConstraint " + content);
  }

  bool match(const BinaryConstraint &other) const {
    return op == other.op &&
           matchOptionalSupportedType(selfType, other.selfType) &&
           (pos == -1 || other.pos == -1 || pos == other.pos) &&
           (!otherExpr.has_value() ||
            !other.otherExpr.has_value() ||
            otherExpr.value() == other.otherExpr.value());
  }

  std::string op;
  std::optional<SupportedType> selfType;
  int pos;
  std::optional<Argum> otherExpr;
};

template <>
struct std::hash<BinaryConstraint> {
  std::size_t operator() (const BinaryConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

bool matchOptionalVector(
  const std::vector<std::optional<Argum>> &v1,
  const std::vector<std::optional<Argum>> &v2
) {
  int l1 = v1.size();
  int l2 = v2.size();
  if (l1 != l2) {
    return false;
  } else {
    for (int i = 0; i < l1; i++) {
      if (v1[i].has_value() && v2[i].has_value()) {
        if (!(v1[i].value() == v2[i].value())) {
          return false;
        }
      }
    }
    return true;
  }
}

std::string optionalVectorToString(
  const std::vector<std::optional<Argum>> &v
) {
  std::string ret;
  for (const auto &e : v) {
    if (ret != "") {
      ret += " ";
    }
    if (e.has_value()) {
      ret += argumToString(e.value());
    } else {
      ret += "_";
    }
  }
  return "[" + ret + "]";
}

struct FunctionConstraint {
  FunctionConstraint(std::optional<SupportedType> s = std::nullopt,
                     std::vector<std::optional<Argum>> a = {},
                     std::optional<std::string> r = std::nullopt)
    : selfType(s), args(a), returnType(r) {}

  bool operator== (const FunctionConstraint &other) const {
    return selfType == other.selfType && args == other.args && returnType == other.returnType;
  }

  std::string toStr() const {
    std::string content = interpolate({
      optionalSupportedTypeToString(selfType),
      optionalVectorToString(args),
      (returnType.has_value() ? returnType.value() : "_")
    });
    return CLASS_STRING("FunctionConstraint " + content);
  }

  bool match(const FunctionConstraint &other) const {
    return matchOptionalSupportedType(selfType, other.selfType) &&
           matchOptionalVector(args, other.args) &&
           (!returnType.has_value() || !other.returnType.has_value() ||
             returnType.value() == other.returnType.value());
  }

  std::optional<SupportedType> selfType;
  std::vector<std::optional<Argum>> args;
  std::optional<std::string> returnType;
};

template <>
struct std::hash<FunctionConstraint> {
  std::size_t operator() (const FunctionConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct MemberConstraint {
  MemberConstraint(std::string m, bool i, std::optional<SupportedType> s = std::nullopt,
                   std::vector<std::optional<Argum>> a = {},
                   std::optional<std::string> r = std::nullopt)
    : mb(std::move(m)), isFun(i), selfType(s), args(a), returnType(r) {}

  bool operator== (const MemberConstraint &other) const {
    return mb == other.mb && isFun == other.isFun && selfType == other.selfType
           && args == other.args && returnType == other.returnType;
  }

  std::string toStr() const {
    std::string content = interpolate({
      mb,
      std::to_string(isFun ? 1 : 0),
      optionalSupportedTypeToString(selfType),
      optionalVectorToString(args),
      (returnType.has_value() ? returnType.value() : "_")
    });
    return CLASS_STRING("MemberConstraint " + content);
  }

  bool match(const MemberConstraint &other) const {
    return mb == other.mb &&
           isFun == other.isFun &&
           matchOptionalSupportedType(selfType, other.selfType) &&
           matchOptionalVector(args, other.args) &&
           (!returnType.has_value() || !other.returnType.has_value() ||
            returnType.value() == other.returnType.value());
  }

  std::string mb;
  bool isFun;
  std::optional<SupportedType> selfType;
  std::vector<std::optional<Argum>> args;
  std::optional<std::string> returnType;
};

template <>
struct std::hash<MemberConstraint> {
  std::size_t operator() (const MemberConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

using BackMap = std::map<const TemplateTypeParmDecl*, SupportedType>;

std::string backMapToString(const BackMap& backMap) {
  std::string content;
  for (const auto &[k, v] : backMap) {
    if (content != "") {
      content += " ";
    }
    content += "(";
    content += k->getNameAsString();
    content += " ";
    content += supportedTypeToString(v);
    content += ")";
  }
  return "{" + content + "}";
}

struct TemplateDependency {
  bool operator== (const TemplateDependency &other) const {
    return ttpdecl == other.ttpdecl && backMap == other.backMap;
  }

  bool operator< (const TemplateDependency &other) const {
    if (ttpdecl == other.ttpdecl) {
      return backMap < other.backMap;
    } else {
      return ttpdecl < other.ttpdecl;
    }
  }

  std::string toStr() const {
    std::string content;
    content += ttpdecl->getNameAsString();
    content += backMapToString(backMap);
    return CLASS_STRING(content);
  }

  const TemplateTypeParmDecl *ttpdecl;
  BackMap backMap;
};

using Dependency = std::variant<QualType, TemplateDependency>;

std::string dependencyToString(const Dependency &d) {
  if (std::holds_alternative<QualType>(d)) {
    return std::get<QualType>(d).getAsString();
  } else {
    return std::get<TemplateDependency>(d).toStr();
  }
}

struct CallConstraint {
  CallConstraint(std::optional<SupportedType> s, std::vector<Dependency> c)
    : selfType(s), dependencies(std::move(c)) {
    std::sort(dependencies.begin(), dependencies.end());
  }

  bool operator== (const CallConstraint &other) const {
    return selfType == other.selfType && dependencies == other.dependencies;
  }

  std::string toStr() const {
    std::string content;
    content += optionalSupportedTypeToString(selfType);
    for (const auto &cc : dependencies) {
      content += " ";
      content += dependencyToString(cc);
    }
    return CLASS_STRING(content);
  }

  std::optional<SupportedType> selfType;
  std::vector<Dependency> dependencies;
};

template <>
struct std::hash<CallConstraint> {
  std::size_t operator() (const CallConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct ConcreteConstraint {
  ConcreteConstraint(std::optional<SupportedType> s, std::optional<SupportedType> c)
    : selfType(s), constraintType(c) {}
  
  bool operator== (const ConcreteConstraint &other) const {
    return selfType == other.selfType && constraintType == other.constraintType;
  }

  std::string toStr() const {
    std::string content;
    content += optionalSupportedTypeToString(selfType);
    content += " ";
    content += optionalSupportedTypeToString(constraintType);
    return CLASS_STRING("ConcreteConstraint " + content);
  }

  std::optional<SupportedType> selfType;
  std::optional<SupportedType> constraintType;
};

template <>
struct std::hash<ConcreteConstraint> {
  std::size_t operator() (const ConcreteConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

using Constraint = std::variant<
  TypeTraitConstraint,
  UnaryConstraint,
  BinaryConstraint,
  FunctionConstraint,
  MemberConstraint,
  CallConstraint,
  ConcreteConstraint
>;

/******************************************************
 * instantiation class
 ******************************************************/

struct Instantiation {
  Instantiation(QualType t)
    : type(t) {}

  bool operator== (const Instantiation &other) const {
    return type == other.type;
  }

  std::string toStr() const {
    return CLASS_STRING(type.getAsString());
  }

  QualType type;
};

template <>
struct std::hash<Instantiation> {
  std::size_t operator() (const Instantiation &i) const {
    return std::hash<std::string>()(i.toStr());
  }
};

/******************************************************
 * simple inference
 ******************************************************/

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

std::optional<std::string> simplyInfer(const Expr *expr, ASTContext *ctx) {
  auto parent = getFirstStmtParent(expr, ctx);
  if (parent) {
    for (const auto &[type, checker] : simpleInferences) {
      if (checker(expr, parent)) {
        return type;
      }
    }
  }
  return std::nullopt;
}

/******************************************************
 * individual function template visitor
 ******************************************************/

struct VariableUseStmt {
  VariableUseStmt(const TemplateTypeParmDecl *tt, const Stmt *v, const Stmt *s, ASTContext *c)
    : ttpdecl(tt), var(v), stmt(s), ctx(c) {}

  std::string toStr() const {
    std::string content = interpolate({
      ttpdecl->getNameAsString(),
      getFullSourceLocationAsString(var, ctx),
      getFullSourceLocationAsString(stmt, ctx)
    });
    return CLASS_STRING(content);
  }

  const TemplateTypeParmDecl *ttpdecl;
  const Stmt *var;
  const Stmt *stmt;
  ASTContext *ctx;
};

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *c, FunctionTemplateDecl *f)
    : context(c), functionTemplateDecl(f) {}

  // Get instantiations
  bool VisitFunctionDecl(FunctionDecl *fdecl) {
    if (fdecl->isTemplateInstantiation()) {
      templateArgumentLists.push_back(fdecl->getTemplateSpecializationArgs());
    }
    return true;
  }

  // Get template body usages
  bool VisitDeclRefExpr(DeclRefExpr *var) {
    // ignore compound template types like T*, std::vector<T>, etc.
    if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(
      var->getType(),
      functionTemplateDecl->getTemplateParameters(),
      context
    )) {
      if (auto ps = getFirstStmtParent(var, context)) {
        variableUseStmts.push_back(VariableUseStmt(ttpdecl, var, ps, context));
        // { special case: ignore std::move and std::forward
        VariableUseStmt &ref = variableUseStmts.back();
        if (isMoveOrForward(ref.stmt)) {
          if (auto pps = getFirstStmtParent(ref.stmt, ref.ctx)) {
            ref.var = ref.stmt;
            ref.stmt = pps;
          } else {
            variableUseStmts.pop_back();
          }
        }
        // special case }
      }
    }
    return true;
  }

  // Get template body trait constraints
  bool visitStaticAssertDecl(StaticAssertDecl *sad) {
    auto expr = sad->getAssertExpr();
    auto tc = tryTraitConstraint(
      expr,
      functionTemplateDecl->getTemplateParameters(),
      context
    );
    if (tc.has_value()) {
      typeTraitConstraints.push_back(tc.value());
    }
    return true;
  }

  bool shouldVisitTemplateInstantiations() const {
    return true;
  }

  std::vector<const TemplateArgumentList *> templateArgumentLists;
  std::vector<VariableUseStmt> variableUseStmts;
  std::vector<TypeTraitConstraint> typeTraitConstraints;

private:

  ASTContext *context;
  FunctionTemplateDecl *functionTemplateDecl;
};

/******************************************************
 * translation unit visitor
 ******************************************************/

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *ftdecl) {
    // ignore deduction guides
    if (
      ftdecl->getKind() == Decl::Kind::CXXDeductionGuide ||
      (ftdecl->getAsFunction() && ftdecl->getAsFunction()->getKind() == Decl::Kind::CXXDeductionGuide)
    ) {
      return true;
    }
    // exclude class / instance members
    if (ftdecl->isCXXClassMember() || ftdecl->isCXXInstanceMember()) {
      return true;
    }
    // exclude variadic templates
    if (isVariadicFunctionTemplate(ftdecl)) {
      return true;
    }
    TemplateParameterList *tplist = ftdecl->getTemplateParameters();
    // initialize constraintMap
    for (auto p = tplist->begin(); p != tplist->end(); p++) {
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(*p)) {
        constraintMap[ttpdecl].clear();
      }
    }
    // traverse
    TraverseFunctionTemplateVisitor visitor(context, ftdecl);
    visitor.TraverseDecl(ftdecl);
    // get type trait constraints
    for (auto pit = ftdecl->getAsFunction()->param_begin();
         pit != ftdecl->getAsFunction()->param_end();
         pit++) {
      if (auto e = getEnableIfBoolExpr((*pit)->getType())) {
        auto t = tryTraitConstraint(e, tplist, context);
        if (t.has_value()) {
          constraintMap[t.value().ttpdecl].insert(t.value());
        }
      }
    }
    if (auto e = getEnableIfBoolExpr(ftdecl->getAsFunction()->getReturnType())) {
      auto t = tryTraitConstraint(e, tplist, context);
      if (t.has_value()) {
        constraintMap[t.value().ttpdecl].insert(t.value());
      }
    }
    // handle template-typed variable usages
    for (auto varUseExpr : visitor.variableUseStmts) {
      if (auto unaryOp = dyn_cast<UnaryOperator>(varUseExpr.stmt)) {
        constraintMap[varUseExpr.ttpdecl].insert(
          UnaryConstraint(
            unaryOp->getOpcodeStr(unaryOp->getOpcode()).str(),
            varUseExpr.ttpdecl,
            ((unaryOp->isPostfix()) ? 0 : 1)
          )
        );
      } else if (auto binaryOp = dyn_cast<BinaryOperator>(varUseExpr.stmt)) {
        auto otherHand = 
          binaryOp->getLHS() == varUseExpr.var ?
          binaryOp->getRHS() :
          binaryOp->getLHS();
        constraintMap[varUseExpr.ttpdecl].insert(
          BinaryConstraint(
            binaryOp->getOpcodeStr(binaryOp->getOpcode()).str(),
            varUseExpr.ttpdecl,
            (binaryOp->getLHS() == varUseExpr.var ? 0 : 1),
            tryArgum(otherHand, tplist, context)
          )
        );
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.stmt)) {
        // ignore overloaded operators
        if (dyn_cast<CXXOperatorCallExpr>(varUseExpr.stmt)) {
          continue;
        }
        // var used as function
        if (dyn_cast<DeclRefExpr>(callExpr->getCallee()) == varUseExpr.var) {
          std::vector<std::optional<Argum>> args;
          for (auto arg = callExpr->arg_begin(); arg != callExpr->arg_end(); arg++) {
            args.push_back(tryArgum(*arg, tplist, context));
          }
          constraintMap[varUseExpr.ttpdecl].insert(
            FunctionConstraint(
              varUseExpr.ttpdecl,
              args,
              simplyInfer(callExpr, context)
            )
          );
        // var used as argument && callee is an identifier
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
          std::vector<Dependency> dependencies;
          bool hasUnhandledCandidate = false;
          int nArgs = callExpr->getNumArgs();
          for (auto declit = namedCallee->decls_begin(); declit != namedCallee->decls_end(); declit++) {
            auto canddecl = *declit;
            // ignore class / instance members
            if (canddecl->isCXXClassMember() || canddecl->isCXXInstanceMember()) {
              hasUnhandledCandidate = true;
              break;
            }
            // candidate is a function template
            if (auto callee_ftdecl = dyn_cast<FunctionTemplateDecl>(canddecl)) {
              // ignore variadic templates
              if (isVariadicFunctionTemplate(callee_ftdecl)) {
                hasUnhandledCandidate = true;
                break;
              }
              if (isValidNumberOfArgs(callee_ftdecl, nArgs)) {
                auto callee_parmtype = callee_ftdecl->getAsFunction()->getParamDecl(pos)->getType();
                if (callee_parmtype->isDependentType()) {
                  auto callee_tplist = callee_ftdecl->getTemplateParameters();
                  if (auto callee_ttpdecl = fromQualTypeToTemplateTypeParmDecl(
                    callee_parmtype,
                    callee_tplist,
                    context
                  )) {
                    TemplateDependency dependency;
                    dependency.ttpdecl = callee_ttpdecl;
                    // best effort: fill backMap
                    for (int i = 0; i < nArgs; i++) {
                      // left: callsite arg type
                      std::optional<SupportedType> left = std::nullopt;
                      // right: callee parm type's template type parm decl
                      const TemplateTypeParmDecl *right = nullptr;
                      if (i == pos) { // special case: for current position the left is just ttpdecl
                        left = varUseExpr.ttpdecl;
                      } else {
                        const Expr *arg = callExpr->getArg(i);
                        if (auto plain = trySupportedType(arg->getType(), tplist, context)) {
                          left = plain;
                        } else if (isMoveOrForward(arg)) {
                          if (auto mf = trySupportedType(
                            dyn_cast<CallExpr>(arg)->getArg(0)->getType(),
                            tplist,
                            context
                          )) {
                            left = mf;
                          }
                        }
                      }
                      right = fromQualTypeToTemplateTypeParmDecl(
                        callee_ftdecl->getAsFunction()->getParamDecl(i)->getType(),
                        callee_tplist,
                        context
                      );
                      if (left.has_value() && right) {
                        dependency.backMap[right] = left.value();
                      }
                    }
                    dependencies.push_back(dependency);
                  } else {
                    hasUnhandledCandidate = true;
                    break;
                  }
                } else {
                  dependencies.push_back(callee_parmtype);
                }
                // specializations (C++ only allows full specialization for function templates)
                for (auto specit = callee_ftdecl->spec_begin(); specit != callee_ftdecl->spec_end(); specit++) {
                  auto spec = *specit;
                  if (!(spec->isTemplateInstantiation())) {
                    dependencies.push_back(spec->getParamDecl(pos)->getType());
                  }
                }
              } // else: number of args doesn't match, can safely ignore
            // candidate is a function
            } else if (auto callee_fdecl = dyn_cast<FunctionDecl>(canddecl)) {
              // ignore variadic functions
              if (callee_fdecl->isVariadic()) {
                hasUnhandledCandidate = true;
                break;
              }
              if (isValidNumberOfArgs(callee_fdecl, nArgs)) {
                dependencies.push_back(callee_fdecl->getParamDecl(pos)->getType());
              } // else: number of args doesn't match, can safely ignore
            // not sure what the candidate is
            } else {
              hasUnhandledCandidate = true;
              break;
            }
          }
          // only treat it as a constraint when every case is handled
          if (!hasUnhandledCandidate) {
            constraintMap[varUseExpr.ttpdecl].insert(
              CallConstraint(varUseExpr.ttpdecl, std::move(dependencies))
            );
          }
        } // else: ignore this callsite
      } else if (auto mexpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.stmt)) {
        // ignore member accesses via ->
        if (mexpr->isArrow()) {
          continue;
        }
        auto parent = getFirstStmtParent(mexpr, context);
        auto possibleMemberCall = parent ? dyn_cast<CallExpr>(parent) : nullptr;
        // this is a member function
        if (possibleMemberCall && possibleMemberCall->getCallee() == mexpr) {
          std::vector<std::optional<Argum>> args;
          for (auto arg = possibleMemberCall->arg_begin(); arg != possibleMemberCall->arg_end(); arg++) {
            args.push_back(tryArgum(*arg, tplist, context));
          }
          constraintMap[varUseExpr.ttpdecl].insert(
            MemberConstraint(
              mexpr->getMemberNameInfo().getAsString(),
              true,
              varUseExpr.ttpdecl,
              args,
              simplyInfer(possibleMemberCall, context)
            )
          );
        // this is a member variable
        } else {
          constraintMap[varUseExpr.ttpdecl].insert(
            MemberConstraint(
              mexpr->getMemberNameInfo().getAsString(),
              false,
              varUseExpr.ttpdecl,
              {},
              simplyInfer(mexpr, context)
            )
          );
        }
      }
    }

    // handle instantiations
    int len = tplist->size();
    for (int i = 0; i < len; i++) {
      auto decl = tplist->getParam(i);
      if (auto ttpdecl = dyn_cast<TemplateTypeParmDecl>(decl)) {
        for (auto argList : visitor.templateArgumentLists) {
          Instantiation insta((*argList)[i].getAsType());
          instantiationMap[ttpdecl].insert(insta);
        }
      }
    }
  
    return true;
  }

  std::unordered_map<const TemplateTypeParmDecl*, std::unordered_set<Constraint>> constraintMap;
  std::unordered_map<const TemplateTypeParmDecl*, std::unordered_set<Instantiation>> instantiationMap;

private:
  ASTContext *context;
};

using AtomicConstraint = std::variant<
  ConcreteConstraint,
  TypeTraitConstraint,
  UnaryConstraint,
  BinaryConstraint,
  FunctionConstraint,
  MemberConstraint
>;

/******************************************************
* memory management
*******************************************************/

template <typename B>
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

  template <typename D, typename... Args>
  D *poolNew(Args&&... args) {
    auto ptr = new D(std::forward<Args>(args)...);
    pointers.push_back(ptr);
    return ptr;
  }

private:
  std::vector<B*> pointers;
};

/******************************************************
 * code classes
 * each analyzed function template gets its own separated code tree
 ******************************************************/
// code's structrue tag
enum class STag {
  E, // error
  A, // atom
  C, // conjunction
  D  // disjunction
};

struct ConstraintCode {
  ConstraintCode() = default;

  ConstraintCode(const ConstraintCode &c) = delete;

  ConstraintCode &operator= (const ConstraintCode &c) = delete;

  virtual ~ConstraintCode() {}

  virtual bool isValid() const {
    return true;
  }

  virtual std::string toStr() const {
    return "";
  }

  virtual STag getSTag() const {
    return STag::E;
  }

  virtual std::vector<ConstraintCode*> getSubtrees() const {
    return {};
  }

  virtual ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const {
    return nullptr;
  }
};

struct LiteralConstraintCode : public ConstraintCode {
  LiteralConstraintCode() = default;

  LiteralConstraintCode(const LiteralConstraintCode &c) = delete;

  LiteralConstraintCode &operator= (const LiteralConstraintCode &c) = delete;

  ~LiteralConstraintCode() override {}

  bool isValid() const override {
    return value != "";
  }

  std::string toStr() const override {
    return value;
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto l = cpool.poolNew<LiteralConstraintCode>();
    l->value = value;
    return l;
  }

  std::string value;
};

struct ConcreteConstraintCode : public ConstraintCode {
  ConcreteConstraintCode() = default;

  ConcreteConstraintCode(const ConcreteConstraintCode &c) = delete;

  ConcreteConstraintCode &operator= (const ConcreteConstraintCode &c) = delete;

  ~ConcreteConstraintCode() override {}

  bool isValid() const override {
    return selfType != "" && targetType != "";
  }

  std::string toStr() const override {
    return stringFormat("std::convertible_to<#, #>", {selfType, targetType});
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto c = cpool.poolNew<ConcreteConstraintCode>();
    c->selfType = selfType;
    c->targetType = targetType;
    return c;
  }

  std::string selfType;
  std::string targetType;
};

struct TypeTraitConstraintCode : public ConstraintCode {
  TypeTraitConstraintCode() = default;

  TypeTraitConstraintCode(const TypeTraitConstraintCode &c) = delete;

  TypeTraitConstraintCode &operator= (const TypeTraitConstraintCode &c) = delete;

  ~TypeTraitConstraintCode() override {}

  bool isValid() const override {
    return selfType != "" && possiblyNegatedPredicate != "";
  }

  std::string toStr() const override {
    return stringFormat("#<#>::value", {possiblyNegatedPredicate, selfType});
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto t = cpool.poolNew<TypeTraitConstraintCode>();
    t->selfType = selfType;
    t->possiblyNegatedPredicate = possiblyNegatedPredicate;
    return t;
  }

  std::string selfType;
  std::string possiblyNegatedPredicate;
};

struct UnaryConstraintCode : public ConstraintCode {
  UnaryConstraintCode() = default;

  UnaryConstraintCode(const UnaryConstraintCode &c) = delete;

  UnaryConstraintCode &operator= (const UnaryConstraintCode &c) = delete;

  ~UnaryConstraintCode() override {}

  bool isValid() const override {
    return selfType != "" && operatorName != "" && position > -1;
  }

  std::string toStr() const override {
    auto expr = ((position == 0) ? ("x0" + operatorName) : (operatorName + "x0"));
    return stringFormat("requires (# x0) { #; }", {selfType, expr});
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto u = cpool.poolNew<UnaryConstraintCode>();
    u->selfType = selfType;
    u->operatorName = operatorName;
    u->position = position;
    return u;
  }

  std::string selfType;
  std::string operatorName;
  int position;
};

struct BinaryConstraintCode : public ConstraintCode {
  BinaryConstraintCode() = default;

  BinaryConstraintCode(const BinaryConstraintCode &c) = delete;

  BinaryConstraintCode &operator= (const BinaryConstraintCode &c) = delete;

  ~BinaryConstraintCode() override {}

  bool isValid() const override {
    return selfType != "" && operatorName != "" && position > -1 &&
           otherType != "" && otherExpr != "";
  }

  std::string toStr() const override {
    auto expr = ((position == 0) ?
                 ("x0 " + operatorName + " " + otherExpr) :
                 (otherExpr + " " + operatorName + " x0"));
    return stringFormat("requires (# x0, # x1) { #; }", {selfType, otherType, expr});
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto b = cpool.poolNew<BinaryConstraintCode>();
    b->selfType = selfType;
    b->operatorName = operatorName;
    b->position = position;
    b->otherType = otherType;
    b->otherExpr = otherExpr;
    return b;
  }

  std::string selfType;
  std::string operatorName;
  int position;
  std::string otherType;
  std::string otherExpr;
};

struct FunctionConstraintCode : public ConstraintCode {
  FunctionConstraintCode() = default;

  FunctionConstraintCode(const FunctionConstraintCode &c) = delete;

  FunctionConstraintCode &operator= (const FunctionConstraintCode &c) = delete;

  ~FunctionConstraintCode() override {}

  bool isValid() const override {
    if (selfType == "") {
      return false;
    }
    for (const std::string &pt : parameterTypes) {
      if (pt == "") {
        return false;
      }
    }
    for (const std::string &a : args) {
      if (a == "") {
        return false;
      }
    }
    // do not require returnType
    return true;
  }

  std::string toStr() const override {
    int np = parameterTypes.size();
    std::string pattern = "requires (# f";
    for (int i = 0; i < np; i++) {
      pattern += (", # x" + std::to_string(i));
    }
    pattern += ") { #; }";
    std::string arglist = "";
    for (int i = 0; i < np; i++) {
      if (arglist != "") {
        arglist += ", ";
      }
      arglist += (args[i]);
    }
    std::string call = "f(" + arglist + ")";
    if (returnType != "") {
      call = "{" + call + "} -> " + "std::convertible_to<" + returnType + ">";
    }
    std::vector<std::string> elements;
    elements.push_back(selfType);
    for (const std::string &t : parameterTypes) {
      elements.push_back(t);
    }
    elements.push_back(call);
    return stringFormat(pattern, elements);
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto f = cpool.poolNew<FunctionConstraintCode>();
    f->selfType = selfType;
    f->parameterTypes = parameterTypes;
    f->args = args;
    f->returnType = returnType;
    return f;
  }

  std::string selfType;
  std::vector<std::string> parameterTypes;
  std::vector<std::string> args;
  std::string returnType;
};

struct MemberConstraintCode : public ConstraintCode {
  MemberConstraintCode() = default;

  MemberConstraintCode(const MemberConstraintCode &c) = delete;

  MemberConstraintCode &operator= (const MemberConstraintCode &c) = delete;

  ~MemberConstraintCode() override {}

  bool isValid() const override {
    if (selfType == "") {
      return false;
    }
    if (memberName == "") {
      return false;
    }
    for (const std::string &pt : parameterTypes) {
      if (pt == "") {
        return false;
      }
    }
    for (const std::string &a : args) {
      if (a == "") {
        return false;
      }
    }
    // do not require returnType
    return true;
  }

  std::string toStr() const override {
    int np = parameterTypes.size();
    std::string pattern = "requires (# o";
    for (int i = 0; i < np; i++) {
      pattern += (", # x" + std::to_string(i));
    }
    pattern += ") { #; }";
    std::string access = "o." + memberName;
    if (isFun) {
      access += "(";
      std::string arglist;
      for (int i = 0; i < np; i++) {
        if (arglist != "") {
          arglist += ", ";
        }
        arglist += args[i];
      }
      access += arglist;
      access += ")";
    }
    if (returnType != "") {
      access = "{" + access + "} -> " + "std::convertible_to<" + returnType + ">";
    }
    std::vector<std::string> elements;
    elements.push_back(selfType);
    for (const std::string &t : parameterTypes) {
      elements.push_back(t);
    }
    elements.push_back(access);
    return stringFormat(pattern, elements);
  }

  STag getSTag() const override {
    return STag::A;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return {};
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    auto m = cpool.poolNew<MemberConstraintCode>();
    m->selfType = selfType;
    m->memberName = memberName;
    m->isFun = isFun;
    m->parameterTypes = parameterTypes;
    m->args = args;
    m->returnType = returnType;
    return m;
  }

  std::string selfType;
  std::string memberName;
  bool isFun;
  std::vector<std::string> parameterTypes;
  std::vector<std::string> args;
  std::string returnType;
};

struct ConjunctionConstraintCode : public ConstraintCode {
  ConjunctionConstraintCode() = default;

  ConjunctionConstraintCode(const ConjunctionConstraintCode &c) = delete;

  ConjunctionConstraintCode &operator= (const ConjunctionConstraintCode &c) = delete;

  ~ConjunctionConstraintCode() override { /* don't release children's memory here */ }

  bool isValid() const override {
    for (auto c : conjuncts) {
      if (!(c->isValid())) {
        return false;
      }
    }
    return true;
  }

  std::string toStr() const override {
    std::string result = "";
    for (ConstraintCode *c : conjuncts) {
      if (result != "") {
        result += " &&\n";
      }
      result += c->toStr();
    }
    if (result == "") {
      return "(true)";
    } else {
      return replaceAll("(\n" + result, '\n', "\n ") + "\n)";
    }
  }

  STag getSTag() const override {
    return STag::C;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return conjuncts;
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    std::vector<ConstraintCode*> newConjuncts;
    int cnt = 0;
    std::unordered_set<std::string> deduplicate;
    for (auto c : conjuncts) {
      std::vector<ConstraintCode*> targets;
      auto sc = c->getSimplified(cpool);
      if (sc->getSTag() == STag::C) {
        for (auto child : sc->getSubtrees()) {
          targets.push_back(child);
        }
      } else {
        targets.push_back(sc);
      }
      for (auto target : targets) {
        std::string s = target->toStr();
        if (s == "true") {
          continue;
        }
        if (s == "false") {
          auto l = cpool.poolNew<LiteralConstraintCode>();
          l->value = "false";
          return l;
        }
        if (deduplicate.count(s) > 0) {
          continue;
        }
        newConjuncts.push_back(target);
        cnt++;
        deduplicate.insert(s);
      }
    }
    if (cnt == 0) {
      auto l = cpool.poolNew<LiteralConstraintCode>();
      l->value = "true";
      return l;
    } else if (cnt == 1) {
      return newConjuncts.front();
    } else {
      auto c = cpool.poolNew<ConjunctionConstraintCode>();
      c->conjuncts = newConjuncts;
      return c;
    }
  }

  std::vector<ConstraintCode*> conjuncts;
};

struct DisjunctionConstraintCode : public ConstraintCode {
  DisjunctionConstraintCode() = default;

  DisjunctionConstraintCode(const DisjunctionConstraintCode &c) = delete;

  DisjunctionConstraintCode &operator= (const DisjunctionConstraintCode &c) = delete;

  ~DisjunctionConstraintCode() override { /* don't release children's memory here */ }

  bool isValid() const override {
    for (auto c : disjuncts) {
      if (!(c->isValid())) {
        return false;
      }
    }
    return true;
  }

  std::string toStr() const override {
    std::string result = "";
    for (ConstraintCode *d : disjuncts) {
      if (result != "") {
        result += " ||\n";
      }
      result += d->toStr();
    }
    if (result == "") {
      return "(false)";
    } else {
      return replaceAll("(\n" + result, '\n', "\n ") + "\n)";
    }
  }

  STag getSTag() const override {
    return STag::D;
  }

  std::vector<ConstraintCode*> getSubtrees() const override {
    return disjuncts;
  }

  ConstraintCode *getSimplified(Pool<ConstraintCode> &cpool) const override {
    std::vector<ConstraintCode*> newDisjuncts;
    int cnt = 0;
    std::unordered_set<std::string> deduplicate;
    for (auto d : disjuncts) {
      std::vector<ConstraintCode*> targets;
      auto sd = d->getSimplified(cpool);
      if (sd->getSTag() == STag::D) {
        for (auto child : sd->getSubtrees()) {
          targets.push_back(child);
        }
      } else {
        targets.push_back(sd);
      }
      for (auto target : targets) {
        std::string s = target->toStr();
        if (s == "false") {
          continue;
        }
        if (s == "true") {
          auto l = cpool.poolNew<LiteralConstraintCode>();
          l->value = "true";
          return l;
        }
        if (deduplicate.count(s) > 0) {
          continue;
        }
        newDisjuncts.push_back(target);
        cnt++;
        deduplicate.insert(s);
      }
    }
    if (cnt == 0) {
      auto l = cpool.poolNew<LiteralConstraintCode>();
      l->value = "false";
      return l;
    } else if (cnt == 1) {
      return newDisjuncts.front();
    } else {
      auto d = cpool.poolNew<DisjunctionConstraintCode>();
      d->disjuncts = newDisjuncts;
      return d;
    }
  }

  std::vector<ConstraintCode*> disjuncts;
};

/******************************************************
 * formula classes
 * the entire translation unit share the same formula DAG
 * in detail: if both f<>() and g<>() calls h<>(), then both f<>() and g<>() point to h<>()'s formula
 ******************************************************/

class Formula {
public:
  Formula() = default;

  Formula(const Formula &f) = delete;

  Formula &operator= (const Formula &f) = delete;

  virtual ~Formula() {}

  virtual std::string toStr() const {
    return CLASS_STRING("");
  }

  virtual int literalStatus() const {
    return -1;
  }

  virtual bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const {
    return true;
  }

  virtual ConstraintCode *printConstraintCode(
    std::vector<const BackMap*> &backMaps,
    Pool<ConstraintCode> &cpool
  ) const {
    return nullptr;
  }
};

class Literal : public Formula {
public:
  Literal(bool v) : value(v) {}

  Literal(const Literal &l) = delete;

  Literal &operator= (const Literal &l) = delete;

  ~Literal() override {}

  std::string toStr() const override {
    if (value) {
      return CLASS_STRING("true");
    } else {
      return CLASS_STRING("false");
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

  ConstraintCode *printConstraintCode(
    std::vector<const BackMap*> &backMaps,
    Pool<ConstraintCode> &cpool
  ) const override {
    auto l = cpool.poolNew<LiteralConstraintCode>();
    l->value = value ? "true" : "false";
    return l;
  }

  bool value;
};

class Atomic : public Formula {
public:
  Atomic(const AtomicConstraint &c) : con(c) {}

  Atomic(const Atomic &a) = delete;

  Atomic &operator= (const Atomic &a) = delete;

  ~Atomic() override {}

  std::string toStr() const override {
    if (std::holds_alternative<ConcreteConstraint>(con)) {
      ConcreteConstraint c = std::get<ConcreteConstraint>(con);
      return CLASS_STRING(c.toStr());
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      return CLASS_STRING(u.toStr());
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      return CLASS_STRING(b.toStr());
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      return CLASS_STRING(f.toStr());
    } else if (std::holds_alternative<MemberConstraint>(con)) {
      MemberConstraint m = std::get<MemberConstraint>(con);
      return CLASS_STRING(m.toStr());
    } else {
      TypeTraitConstraint t = std::get<TypeTraitConstraint>(con);
      return CLASS_STRING(t.toStr());
    }
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    return has_constraint(con);
  }

  ConstraintCode *printConstraintCode(
    std::vector<const BackMap*> &backMaps,
    Pool<ConstraintCode> &cpool
  ) const override {
    // if printing constraint for f() and f() calls g()
    // then f() might depend on g()'s constraints
    // but g()'s constraints could be parameterized by g()'s template type parameters
    // in such cases we resolve g()'s template type parameters
    // to either f()'s template type parameters or non-dependent types
    auto resolveType = [&](const std::optional<SupportedType> &t) -> std::string {
      if (t.has_value()) {
        auto st = t.value();
        if (std::holds_alternative<QualType>(st)) {
          return std::get<QualType>(st).getAsString();
        } else {
          auto ttpdecl = std::get<const TemplateTypeParmDecl*>(st);
          for (auto backMapPtr = backMaps.rbegin(); backMapPtr != backMaps.rend(); backMapPtr++) {
            if ((**backMapPtr).count(ttpdecl) == 0) {
              return "";
            } else {
              auto next = (**backMapPtr).at(ttpdecl);
              if (std::holds_alternative<QualType>(next)) {
                return std::get<QualType>(next).getAsString();
              } else {
                ttpdecl = std::get<const TemplateTypeParmDecl*>(next);
              }
            }
          }
          return ttpdecl->getNameAsString();
        }
      } else {
        return "";
      }
    };

#define CHECK_RETURN(c) do {\
  if ((c)->isValid()) {\
    return (c);\
  } else {\
    auto t = cpool.poolNew<LiteralConstraintCode>();\
    t->value = "true";\
    return t;\
  }\
} while (0)

    if (std::holds_alternative<ConcreteConstraint>(con)) {
      ConcreteConstraint c = std::get<ConcreteConstraint>(con);
      auto ccc = cpool.poolNew<ConcreteConstraintCode>();
      ccc->selfType = resolveType(c.selfType);
      ccc->targetType = resolveType(c.constraintType);
      CHECK_RETURN(ccc);
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      auto ucc = cpool.poolNew<UnaryConstraintCode>();
      ucc->operatorName = u.op;
      ucc->position = u.pos;
      ucc->selfType = resolveType(u.selfType);
      CHECK_RETURN(ucc);
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      auto bcc = cpool.poolNew<BinaryConstraintCode>();
      bcc->operatorName = b.op;
      bcc->position = b.pos;
      bcc->selfType = resolveType(b.selfType);
      if (b.otherExpr.has_value()) {
        const Argum &e = b.otherExpr.value();
        if (std::holds_alternative<QualType>(e)) {
          bcc->otherType = std::get<QualType>(e).getAsString();
          bcc->otherExpr = "x1";
        } else {
          const DependentExpression &de = std::get<DependentExpression>(e);
          bcc->otherType = resolveType(de.ttpdecl);
          bcc->otherExpr = ((de.pos == 0) ? ("x1" + de.op) : (de.op + "x1"));
        }
      }
      CHECK_RETURN(bcc);
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      auto fcc = cpool.poolNew<FunctionConstraintCode>();
      fcc->selfType = resolveType(f.selfType);
      int i = 0;
      for (const auto &a : f.args) {
        auto varName = "x" + std::to_string(i);
        if (a.has_value()) {
          const Argum &e = a.value();
          if (std::holds_alternative<QualType>(e)) {
            fcc->parameterTypes.push_back(std::get<QualType>(e).getAsString());
            fcc->args.push_back(varName);
          } else {
            const DependentExpression &de = std::get<DependentExpression>(e);
            fcc->parameterTypes.push_back(resolveType(de.ttpdecl));
            fcc->args.push_back((de.pos == 0) ? (varName + de.op) : (de.op + varName));
          }
        } else {
          fcc->parameterTypes.push_back("");
          fcc->args.push_back("");
        }
        i++;
      }
      if (f.returnType.has_value()) {
        fcc->returnType = f.returnType.value();
      }
      CHECK_RETURN(fcc);
    } else if (std::holds_alternative<MemberConstraint>(con)) {
      MemberConstraint m = std::get<MemberConstraint>(con);
      auto mcc = cpool.poolNew<MemberConstraintCode>();
      mcc->memberName = m.mb;
      mcc->isFun = m.isFun;
      mcc->selfType = resolveType(m.selfType);
      int i = 0;
      for (const auto &a : m.args) {
        auto varName = "x" + std::to_string(i);
        if (a.has_value()) {
          const Argum &e = a.value();
          if (std::holds_alternative<QualType>(e)) {
            mcc->parameterTypes.push_back(std::get<QualType>(e).getAsString());
            mcc->args.push_back(varName);
          } else {
            const DependentExpression &de = std::get<DependentExpression>(e);
            mcc->parameterTypes.push_back(resolveType(de.ttpdecl));
            mcc->args.push_back((de.pos == 0) ? (varName + de.op) : (de.op + varName));
          }
        } else {
          mcc->parameterTypes.push_back("");
          mcc->args.push_back("");
        }
        i++;
      }
      if (m.returnType.has_value()) {
        mcc->returnType = m.returnType.value();
      }
      CHECK_RETURN(mcc);
    } else if (std::holds_alternative<TypeTraitConstraint>(con)) {
      TypeTraitConstraint t = std::get<TypeTraitConstraint>(con);
      auto tcc = cpool.poolNew<TypeTraitConstraintCode>();
      tcc->selfType = resolveType(t.ttpdecl);
      tcc->possiblyNegatedPredicate = (t.expr.neg ? "!" : "") + t.expr.predicate;
      CHECK_RETURN(tcc);
    } else {
      auto dummy = cpool.poolNew<ConstraintCode>();
      CHECK_RETURN(dummy);
    }

#undef CHECK_RETURN

  }

  AtomicConstraint con;
};

class Conjunction : public Formula {
public:
  Conjunction() = default;

  Conjunction(const Conjunction &c) = delete;

  Conjunction &operator= (const Conjunction &c) = delete;

  ~Conjunction() override { /* don't release children's memory here */ }

  std::string toStr() const override {
    std::string content;
    for (Formula *f : conjuncts) {
      if (content != "") {
        content += " ";
      }
      content += f->toStr();
    }
    return CLASS_STRING(content);
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

  ConstraintCode *printConstraintCode(
    std::vector<const BackMap*> &backMaps,
    Pool<ConstraintCode> &cpool
  ) const override {
    auto ccc = cpool.poolNew<ConjunctionConstraintCode>();
    for (auto f : conjuncts) {
      ccc->conjuncts.push_back(f->printConstraintCode(backMaps, cpool));
    }
    return ccc;
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

  ~Disjunction() override { /* don't release children's memory here */ }

  std::string toStr() const override {
    std::string content;
    for (const std::pair<Formula*, std::optional<BackMap>> &p : disjuncts) {
      if (content != "") {
        content += " ";
      }
      content += p.first->toStr();
      content += " ";
      if (p.second.has_value()) {
        content += backMapToString(p.second.value());
      } else {
        content += "_";
      }
    }
    return CLASS_STRING(content);
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    bool ret = false;
    for (const std::pair<Formula*, std::optional<BackMap>> &p : disjuncts) {
      ret = ret || ((p.first)->evaluate(has_constraint));
    }
    return ret;
  }

  ConstraintCode *printConstraintCode(
    std::vector<const BackMap*> &backMaps,
    Pool<ConstraintCode> &cpool
  ) const override {
    auto dcc = cpool.poolNew<DisjunctionConstraintCode>();
    for (const std::pair<Formula*, std::optional<BackMap>> &p : disjuncts) {
      if (p.second.has_value()) {
        backMaps.push_back(&(p.second.value()));
      }
      dcc->disjuncts.push_back((p.first)->printConstraintCode(backMaps, cpool));
      if (p.second.has_value()) {
        backMaps.pop_back();
      }
    }
    return dcc;
  }

  void addDisjunct(Formula *f, std::optional<BackMap> backMap = std::nullopt) {
    disjuncts.push_back(std::make_pair(f, backMap));
  }

  std::vector<std::pair<Formula*, std::optional<BackMap>>> disjuncts;
};

/******************************************************
 * named requirement inference
 ******************************************************/

namespace namedrequirements {

  struct ConstraintPredicate {
    ConstraintPredicate(const std::vector<AtomicConstraint> &ps) {
      for (const auto &p : ps) {
        if (std::holds_alternative<UnaryConstraint>(p)) {
          unaryPatterns.push_back(std::get<UnaryConstraint>(p));
        } else if (std::holds_alternative<BinaryConstraint>(p)) {
          binaryPatterns.push_back(std::get<BinaryConstraint>(p));
        } else if (std::holds_alternative<FunctionConstraint>(p)) {
          functionPatterns.push_back(std::get<FunctionConstraint>(p));
        } else if (std::holds_alternative<MemberConstraint>(p)) {
          memberPatterns.push_back(std::get<MemberConstraint>(p));
        }
      }
    }
  
    template <typename T>
    bool match(const std::vector<T> &patterns, const T &c) const {
      for (const auto &p : patterns) {
        if (p.match(c)) {
          return true;
        }
      }
      return false;
    }
  
    bool operator() (const AtomicConstraint &c) const {
      if (std::holds_alternative<UnaryConstraint>(c)) {
        return match<UnaryConstraint>(unaryPatterns, std::get<UnaryConstraint>(c));
      } else if (std::holds_alternative<BinaryConstraint>(c)) {
        return match<BinaryConstraint>(binaryPatterns, std::get<BinaryConstraint>(c));
      } else if (std::holds_alternative<FunctionConstraint>(c)) {
        return match<FunctionConstraint>(functionPatterns, std::get<FunctionConstraint>(c));
      } else if (std::holds_alternative<MemberConstraint>(c)) {
        return match<MemberConstraint>(memberPatterns, std::get<MemberConstraint>(c));
      } else { // ConcreteConstraint || TypeTraitConstraint
        return false;
      }
    }
  
    std::vector<UnaryConstraint> unaryPatterns;
    std::vector<BinaryConstraint> binaryPatterns;
    std::vector<FunctionConstraint> functionPatterns;
    std::vector<MemberConstraint> memberPatterns;
  };

  // named requirements ->
  // (has_constraint, has_instantiation)
  std::vector<std::pair<
    std::string,
    std::pair<std::function<bool(const AtomicConstraint&)>, std::function<bool(const Instantiation&)>>
  >> requirements = {
    {
      "Iterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("=")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "InputIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", std::nullopt, 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "OutputIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("="),
          UnaryConstraint("++", std::nullopt, 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "ForwardIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", std::nullopt, 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "BidirectionalIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", std::nullopt, 0),
          UnaryConstraint("--")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "RandomAccessIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", std::nullopt, 1),
          UnaryConstraint("++", std::nullopt, 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", std::nullopt, 0),
          UnaryConstraint("--"),
          BinaryConstraint("+"),
          BinaryConstraint("-"),
          BinaryConstraint("<"),
          BinaryConstraint(">"),
          BinaryConstraint("<="),
          BinaryConstraint(">="),
          BinaryConstraint("+="),
          BinaryConstraint("-=")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "Predicate",
      {
        ConstraintPredicate({
          FunctionConstraint(
            std::nullopt,
            std::vector<std::optional<Argum>>{ std::nullopt },
            "bool"
          )
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "BinaryPredicate",
      {
        ConstraintPredicate({
          FunctionConstraint(
            std::nullopt,
            std::vector<std::optional<Argum>>{ std::nullopt, std::nullopt },
            "bool"
          )
        }),
        [](const Instantiation &i){ return true; }
      }
    }
  };

}

std::vector<std::string> infer(
  const Formula *formula,
  const std::unordered_set<Instantiation> &instantiation_set) {
  std::vector<std::string> requirements;
  // As an extension we may add support of two or more named requirements.
  for (const auto &[name, predicates] : namedrequirements::requirements) {
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
    }
  }
  return requirements;
}

/******************************************************
 * main entry
 ******************************************************/

class ConceptSynthConsumer : public clang::ASTConsumer {
public:
  explicit ConceptSynthConsumer(ASTContext *context) : visitor(context) {}

  virtual void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor.TraverseDecl(context.getTranslationUnitDecl());
    // 0 for not visited, 1 for on stack, 2 for visited
    std::unordered_map<const TemplateTypeParmDecl*, int> status;
    std::unordered_map<const TemplateTypeParmDecl*, Formula*> results;
    Pool<Formula> fpool;
    std::function<Formula*(const TemplateTypeParmDecl*)> dfs =
      [&](const TemplateTypeParmDecl *ttpd) -> Formula* {
      if (status[ttpd] == 0) { // not visited
        status[ttpd] = 1;
        auto conj = fpool.poolNew<Conjunction>();
        for (const auto &con : visitor.constraintMap.at(ttpd)) {
          if (std::holds_alternative<UnaryConstraint>(con)) {
            auto u = fpool.poolNew<Atomic>(std::get<UnaryConstraint>(con));
            conj->addConjunct(u);
          } else if (std::holds_alternative<BinaryConstraint>(con)) {
            auto b = fpool.poolNew<Atomic>(std::get<BinaryConstraint>(con));
            conj->addConjunct(b);
          } else if (std::holds_alternative<MemberConstraint>(con)) {
            auto m = fpool.poolNew<Atomic>(std::get<MemberConstraint>(con));
            conj->addConjunct(m);
          } else if (std::holds_alternative<FunctionConstraint>(con)) {
            auto f = fpool.poolNew<Atomic>(std::get<FunctionConstraint>(con));
            conj->addConjunct(f);
          } else if (std::holds_alternative<TypeTraitConstraint>(con)) {
            auto t = fpool.poolNew<Atomic>(std::get<TypeTraitConstraint>(con));
            conj->addConjunct(t);
          } else if (std::holds_alternative<CallConstraint>(con)) {
            auto c = std::get<CallConstraint>(con);
            auto disj = fpool.poolNew<Disjunction>();
            for (const auto &cc : c.dependencies) {
              if (std::holds_alternative<QualType>(cc)) {
                auto a = fpool.poolNew<Atomic>(ConcreteConstraint(c.selfType, std::get<QualType>(cc)));
                disj->addDisjunct(a);
              } else if (std::holds_alternative<TemplateDependency>(cc)) {
                const auto &td = std::get<TemplateDependency>(cc);
                if (auto f = dfs(td.ttpdecl)) {
                  disj->addDisjunct(f, td.backMap);
                } else { // recursive dependency
                  auto t = fpool.poolNew<Literal>(true);
                  disj->addDisjunct(t);
                }
              }
            }
            conj->addConjunct(disj);
          }
        }
        results[ttpd] = conj;
        status[ttpd] = 2;
        return results.at(ttpd);
      } else if (status[ttpd] == 1) { // on stack
        return nullptr;
      } else { // visited
        return results.at(ttpd);
      }
    };

    for (const auto &kv : visitor.constraintMap) {
      auto ttpd = kv.first;
      if (status[ttpd] == 0) {
        dfs(ttpd);
      }
    }

    for (const auto &kv : results) {
      auto ttpdecl = kv.first;
      auto f = kv.second;
      auto ftdecl = fromTemplateTypeParmDeclToFunctionTemplateDecl(ttpdecl, &context);
      if (ftdecl) {
        llvm::outs() << "[" << ftdecl->getNameAsString() << ":" << ftdecl->getAsFunction()->getNumParams() << ":" << ttpdecl->getNameAsString() << "]\n";
        llvm::outs() << "SourceLocation:\n" << getFullSourceLocationAsString(ftdecl, &context) << "\n";
        Pool<ConstraintCode> cpool;
        std::vector<const BackMap*> backMaps;
        auto cc = f->printConstraintCode(backMaps, cpool);
        auto scc = cc->getSimplified(cpool);
        llvm::outs() << "Printed code (original):\n" << cc->toStr() << "\n";
        llvm::outs() << "Printed code (optimized):\n" << scc->toStr() << "\n";
        llvm::outs() << "Inferred constraint:\n";
        const auto &inferred = infer(f, visitor.instantiationMap[ttpdecl]);
        for (const auto &con : inferred) {
          llvm::outs() << con << " ";
        }
        llvm::outs() << "\n\n";
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
