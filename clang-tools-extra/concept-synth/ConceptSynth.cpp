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

// The core code

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

struct VariableUseStmt {
  VariableUseStmt(const TemplateTypeParmDecl *tt, const DeclRefExpr *v, const Stmt *s, ASTContext *c)
    : ttpdecl(tt), var(v), stmt(s), ctx(c) {}

  std::string toStr() const {
    std::string content = interpolate({
      ttpdecl->getNameAsString(),
      var->getNameInfo().getAsString(),
      getFullSourceLocationAsString(stmt, ctx)
    });
    return CLASS_STRING(content);
  }

  const TemplateTypeParmDecl *ttpdecl;
  const DeclRefExpr *var;
  const Stmt *stmt;
  ASTContext *ctx;
};

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
            if (args.size() > 0) {
              if (args[0].getKind() ==
                  TemplateArgument::ArgKind::Expression) {
                return args[0].getAsExpr();
              }
            }
          }
        }
      }
    }
  }
  return nullptr;
}

struct TypeTraitConstraint {
  TypeTraitConstraint(bool n, std::string p, const TemplateTypeParmDecl *t)
    : neg(n), predicate(std::move(p)), ttpdecl(t) {}

  bool operator== (const TypeTraitConstraint &other) const {
    return neg == other.neg && predicate == other.predicate && ttpdecl == other.ttpdecl;
  }

  std::string toStr() const {
    std::string content;
    content += (neg ? "! " : " ");
    content += predicate;
    content += std::to_string(reinterpret_cast<uintptr_t>(ttpdecl));
    return CLASS_STRING(content);
  }

  bool match(const TypeTraitConstraint &other) const {
    return neg == other.neg && predicate == other.predicate && ttpdecl == other.ttpdecl;
  }

  bool neg;
  std::string predicate;
  const TemplateTypeParmDecl *ttpdecl;
};

template <>
struct std::hash<TypeTraitConstraint> {
  std::size_t operator() (const TypeTraitConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

std::unordered_set<std::string> SupportedTraits {
  "std::is_void", "std::is_null_pointer", "std::is_integral", "std::is_floating_point",
  "std::is_array", "std::is_enum", "std::is_union", "std::is_class", "std::is_function", "std::is_pointer"
};

std::optional<TypeTraitConstraint> trySingletonTrait(
  const Expr *e,
  const TemplateParameterList *tplist,
  ASTContext *ctx) {
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
            if (args.size() > 0) {
              if (args[0].getKind() ==
                  TemplateArgument::ArgKind::Type) {
                if (auto ttpdecl = fromQualTypeToTemplateTypeParmDecl(args[0].getAsType(), tplist, ctx)) {
                  return TypeTraitConstraint(neg, name.value(), ttpdecl);
                }
              }
            }
          }
        }
      }
    }
  }
  return std::nullopt;
}

class TraverseFunctionTemplateVisitor
    : public RecursiveASTVisitor<TraverseFunctionTemplateVisitor> {
public:
  explicit TraverseFunctionTemplateVisitor(ASTContext *c, FunctionTemplateDecl *f)
    : context(c), functionTemplateDeclaration(f) {}

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
      functionTemplateDeclaration->getTemplateParameters(),
      context
    )) {
      if (auto ps = getFirstStmtParent(var, context)) {
        variableUseStmts.push_back(VariableUseStmt(ttpdecl, var, ps, context));
      }
    }
    return true;
  }

  bool visitStaticAssertDecl(StaticAssertDecl *sad) {
    auto e = sad->getAssertExpr();
    auto t = trySingletonTrait(
      e,
      functionTemplateDeclaration->getTemplateParameters(),
      context
    );
    if (t.has_value()) {
      typeTraitConstraints.push_back(t.value());
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
  FunctionTemplateDecl *functionTemplateDeclaration;
};

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

// support non-dependent types and pure template type parameter types
using SupportedType = std::variant<QualType, const TemplateTypeParmDecl*>;

// check for nullptr!
SupportedType trySupportedType(QualType t, const TemplateParameterList *tplist, ASTContext *ctx) {
  if (t->isDependentType()) {
    return fromQualTypeToTemplateTypeParmDecl(t, tplist, ctx);
  } else {
    return t;
  }
}

bool isValidSupportedType(const SupportedType &s) {
  if (std::holds_alternative<const TemplateTypeParmDecl*>(s)) {
    return std::get<const TemplateTypeParmDecl*>(s) != nullptr;
  } else {
    return true;
  }
}

std::string supportedTypeToString(const SupportedType &s) {
  if (std::holds_alternative<QualType>(s)) {
    return std::get<QualType>(s).getAsString();
  } else {
    return std::get<const TemplateTypeParmDecl*>(s)->getNameAsString();
  }
}

using TypeBox = std::variant<std::string, SupportedType>;

bool matchTypeBox(const TypeBox &t1, const TypeBox &t2) {
  if (std::holds_alternative<std::string>(t1) && std::get<std::string>(t1) == "") {
    return true;
  } else if (std::holds_alternative<std::string>(t2) && std::get<std::string>(t2) == "") {
    return true;
  } else {
    return t1 == t2;
  }
}

std::string typeBoxToString(const TypeBox &t) {
  if (std::holds_alternative<std::string>(t)) {
    return std::get<std::string>(t);
  } else {
    return supportedTypeToString(std::get<SupportedType>(t));
  }
}

using TypeBoxVectorBox = std::variant<std::string, std::vector<TypeBox>>;

bool matchTypeBoxVectorBox(const TypeBoxVectorBox &t1, const TypeBoxVectorBox &t2) {
  if (std::holds_alternative<std::string>(t1) && std::get<std::string>(t1) == "") {
    return true;
  } else if (std::holds_alternative<std::string>(t2) && std::get<std::string>(t2) == "") {
    return true;
  } else if (std::holds_alternative<std::string>(t1) && std::holds_alternative<std::string>(t2)) {
    return t1 == t2;
  } else if (std::holds_alternative<std::vector<TypeBox>>(t1) && std::holds_alternative<std::vector<TypeBox>>(t2)) {
    const auto &v1 = std::get<std::vector<TypeBox>>(t1);
    const auto &v2 = std::get<std::vector<TypeBox>>(t2);
    int l1 = v1.size();
    int l2 = v2.size();
    if (l1 != l2) {
      return false;
    }
    for (int i = 0; i < l1; i++) {
      if (!matchTypeBox(v1[i], v2[i])) {
        return false;
      }
    }
    return true;
  } else {
    return false;
  }
}

std::string typeBoxVectorBoxToString(const TypeBoxVectorBox &t) {
  if (std::holds_alternative<std::string>(t)) {
    return "[" + std::get<std::string>(t) + "]";
  } else {
    const auto &v = std::get<std::vector<TypeBox>>(t);
    std::string ret;
    ret += "[";
    for (const auto &b : v) {
      if (ret != "") {
        ret += " ";
      }
      ret += typeBoxToString(b);
    }
    ret += "]";
    return ret;
  }
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

TypeBox simplyInfer(const Expr *e, ASTContext *ctx) {
  TypeBox t = "";
  auto p = getFirstStmtParent(e, ctx);
  if (p) {
    for (const auto &[type, checker] : simpleInferences) {
      if (checker(e, p)) {
        t = type;
        break;
      }
    }
  }
  return t;
}

struct UnaryConstraint {
  UnaryConstraint(std::string o, TypeBox s = "", int p = -1)
    : op(std::move(o)), selfType(s), pos(p) {}

  bool operator== (const UnaryConstraint &other) const {
    return op == other.op && selfType == other.selfType && pos == other.pos;
  }

  std::string toStr() const {
    std::string content = interpolate({op, typeBoxToString(selfType), std::to_string(pos)});
    return CLASS_STRING(content);
  }

  bool match(const UnaryConstraint &other) const {
    return op == other.op &&
           matchTypeBox(selfType, other.selfType) &&
           (pos == -1 || pos == other.pos);
  }

  std::string op;
  TypeBox selfType;
  int pos;
};

template <>
struct std::hash<UnaryConstraint> {
  std::size_t operator() (const UnaryConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct BinaryConstraint {
  BinaryConstraint(std::string o, TypeBox s = "", int p = -1, TypeBox t = "")
    : op(std::move(o)), selfType(s), pos(p), otherType(t) {}

  bool operator== (const BinaryConstraint &other) const {
    return op == other.op && selfType == other.selfType && pos == other.pos && otherType == other.otherType;
  }

  std::string toStr() const {
    std::string content = interpolate({op, typeBoxToString(selfType), std::to_string(pos), typeBoxToString(otherType)});
    return CLASS_STRING(content);
  }

  bool match(const BinaryConstraint &other) const {
    return op == other.op &&
           matchTypeBox(selfType, other.selfType) &&
           (pos == -1 || pos == other.pos) &&
           matchTypeBox(otherType, other.otherType);
  }

  std::string op;
  TypeBox selfType;
  int pos;
  TypeBox otherType;
};

template <>
struct std::hash<BinaryConstraint> {
  std::size_t operator() (const BinaryConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct FunctionConstraint {
  FunctionConstraint(TypeBox s = "", TypeBoxVectorBox p = "", TypeBox r = "")
    : selfType(s), parameterTypes(std::move(p)), returnType(r) {}

  bool operator== (const FunctionConstraint &other) const {
    return selfType == other.selfType && parameterTypes == other.parameterTypes && returnType == other.returnType;
  }

  std::string toStr() const {
    std::string content = interpolate({typeBoxToString(selfType), typeBoxVectorBoxToString(parameterTypes), typeBoxToString(returnType)});
    return CLASS_STRING(content);
  }

  bool match(const FunctionConstraint &other) const {
    return matchTypeBox(selfType, other.selfType) &&
           matchTypeBoxVectorBox(parameterTypes, other.parameterTypes) &&
           matchTypeBox(returnType, other.returnType);
  }

  TypeBox selfType;
  TypeBoxVectorBox parameterTypes;
  TypeBox returnType;
};

template <>
struct std::hash<FunctionConstraint> {
  std::size_t operator() (const FunctionConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct MemberConstraint {
  MemberConstraint(std::string m, bool i, TypeBox s = "", TypeBoxVectorBox p = "", TypeBox r = "")
    : mb(std::move(m)), isFun(i), selfType(s), parameterTypes(std::move(p)), returnType(r) {}

  bool operator== (const MemberConstraint &other) const {
    return mb == other.mb && isFun == other.isFun && selfType == other.selfType
           && parameterTypes == other.parameterTypes && returnType == other.returnType;
  }

  std::string toStr() const {
    std::string content = interpolate({
      mb,
      std::to_string(isFun ? 1 : 0),
      typeBoxToString(selfType),
      typeBoxVectorBoxToString(parameterTypes),
      typeBoxToString(returnType)
    });
    return CLASS_STRING(content);
  }

  bool match(const MemberConstraint &other) const {
    return mb == other.mb &&
           isFun == other.isFun &&
           matchTypeBox(selfType, other.selfType) &&
           matchTypeBoxVectorBox(parameterTypes, other.parameterTypes) &&
           matchTypeBox(returnType, other.returnType);
  }

  std::string mb;
  bool isFun;
  TypeBox selfType;
  TypeBoxVectorBox parameterTypes;
  TypeBox returnType;
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
  content += "[";
  for (const auto &[k, v] : backMap) {
    if (content != "") {
      content += " ";
    }
    if (std::holds_alternative<QualType>(v)) {
      content += (
        "(" +
        k->getNameAsString() +
        " " +
        std::get<QualType>(v).getAsString() +
        ")"
      );
    } else {
      content += (
        "(" +
        k->getNameAsString() +
        " " +
        std::get<const TemplateTypeParmDecl*>(v)->getNameAsString() +
        ")"
      );
    }
  }
  content += "]";
  return content;
}

struct TemplateDependency {
  const TemplateTypeParmDecl *ttpdecl;
  BackMap backMap;
  std::string toStr() const {
    std::string content;
    content += ttpdecl->getNameAsString();
    content += backMapToString(backMap);
    return CLASS_STRING(content);
  }
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
};

using Dependency = std::variant<TypeBox, TemplateDependency>;

std::string dependencyToString(const Dependency &cce) {
  if (std::holds_alternative<TypeBox>(cce)) {
    return typeBoxToString(std::get<TypeBox>(cce));
  } else {
    return std::get<TemplateDependency>(cce).toStr();
  }
}

struct CallConstraint {
  CallConstraint(TypeBox s, std::vector<Dependency> c)
    : selfType(s), dependencies(std::move(c)) {
    std::sort(dependencies.begin(), dependencies.end());
  }

  bool operator== (const CallConstraint &other) const {
    return selfType == other.selfType && dependencies == other.dependencies;
  }

  std::string toStr() const {
    std::string content;
    content += typeBoxToString(selfType);
    for (const auto &cc : dependencies) {
      content += " ";
      content += dependencyToString(cc);
    }
    return CLASS_STRING(content);
  }

  TypeBox selfType;
  std::vector<Dependency> dependencies;
};

template <>
struct std::hash<CallConstraint> {
  std::size_t operator() (const CallConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

struct ConcreteConstraint {
  ConcreteConstraint(TypeBox s, TypeBox c)
    : selfType(s), constraintType(c) {}
  
  bool operator== (const ConcreteConstraint &other) const {
    return selfType == other.selfType && constraintType == other.constraintType;
  }

  std::string toStr() const {
    return CLASS_STRING(typeBoxToString(selfType) + " " + typeBoxToString(constraintType));
  }

  TypeBox selfType;
  TypeBox constraintType;
};

template <>
struct std::hash<ConcreteConstraint> {
  std::size_t operator() (const ConcreteConstraint &c) const {
    return std::hash<std::string>()(c.toStr());
  }
};

using Constraint = std::variant<
  UnaryConstraint,
  BinaryConstraint,
  FunctionConstraint,
  MemberConstraint,
  TypeTraitConstraint,
  CallConstraint,
  ConcreteConstraint
>;

std::string constraintToString(const Constraint& c) {
  if (std::holds_alternative<UnaryConstraint>(c)) {
    return std::get<UnaryConstraint>(c).toStr();
  } else if (std::holds_alternative<BinaryConstraint>(c)) {
    return std::get<BinaryConstraint>(c).toStr();
  } else if (std::holds_alternative<FunctionConstraint>(c)) {
    return std::get<FunctionConstraint>(c).toStr();
  } else if (std::holds_alternative<MemberConstraint>(c)) {
    return std::get<MemberConstraint>(c).toStr();
  } else if (std::holds_alternative<TypeTraitConstraint>(c)) {
    return std::get<TypeTraitConstraint>(c).toStr();
  } else if (std::holds_alternative<CallConstraint>(c)) {
    return std::get<CallConstraint>(c).toStr();
  } else {
    return std::get<ConcreteConstraint>(c).toStr();
  }
}

class FindTargetVisitor : public RecursiveASTVisitor<FindTargetVisitor> {
public:
  explicit FindTargetVisitor(ASTContext *context) : context(context) {}

  bool VisitFunctionTemplateDecl(FunctionTemplateDecl *ftdecl) {
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
        auto t = trySingletonTrait(e, tplist, context);
        if (t.has_value()) {
          constraintMap[t.value().ttpdecl].insert(t.value());
        }
      }
    }
    if (auto e = getEnableIfBoolExpr(ftdecl->getAsFunction()->getReturnType())) {
      auto t = trySingletonTrait(e, tplist, context);
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
        auto otherType = trySupportedType(
          binaryOp->getLHS() == varUseExpr.var ?
          binaryOp->getRHS()->getType() :
          binaryOp->getLHS()->getType(), tplist, context);
        if (isValidSupportedType(otherType)) {
          constraintMap[varUseExpr.ttpdecl].insert(
            BinaryConstraint(
              binaryOp->getOpcodeStr(binaryOp->getOpcode()).str(),
              varUseExpr.ttpdecl,
              (binaryOp->getLHS() == varUseExpr.var ? 0 : 1),
              otherType
            )
          );
        }
      } else if (auto callExpr = dyn_cast<CallExpr>(varUseExpr.stmt)) {
        // ignore overloaded operators
        if (dyn_cast<CXXOperatorCallExpr>(varUseExpr.stmt)) {
          continue;
        }
        // var used as function
        if (auto callable = dyn_cast<DeclRefExpr>(callExpr->getCallee())) {
          if (callable == varUseExpr.var) {
            bool hasUnsupported = false;
            std::vector<TypeBox> parameterTypes;
            for (auto arg = callExpr->arg_begin(); arg != callExpr->arg_end(); arg++) {
              auto argType = trySupportedType((*arg)->getType(), tplist, context);
              if (isValidSupportedType(argType)) {
                parameterTypes.push_back(argType);
              } else {
                hasUnsupported = true;
                break;
              }
            }
            if (!hasUnsupported) {
              TypeBox returnType = simplyInfer(callExpr, context);
              constraintMap[varUseExpr.ttpdecl].insert(
                FunctionConstraint(
                  varUseExpr.ttpdecl,
                  std::move(parameterTypes),
                  returnType
                )
              );
            }
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
                auto callee_tplist = callee_ftdecl->getTemplateParameters();
                auto callee_type = trySupportedType(
                  callee_ftdecl->getAsFunction()->getParamDecl(pos)->getType(),
                  callee_tplist, context);
                if (isValidSupportedType(callee_type)) {
                  if (std::holds_alternative<const TemplateTypeParmDecl*>(callee_type)) {
                    TemplateDependency dependency;
                    dependency.ttpdecl = std::get<const TemplateTypeParmDecl*>(callee_type);
                    // best effort: fill backMap
                    for (int i = 0; i < nArgs; i++) {
                      auto callee_parmtype = callee_ftdecl->getAsFunction()->getParamDecl(i)->getType();
                      if (auto callee_ttpdecl = fromQualTypeToTemplateTypeParmDecl(callee_parmtype, callee_tplist, context)) {
                        auto argType = trySupportedType(callExpr->getArg(i)->getType(), tplist, context);
                        if (isValidSupportedType(argType)) {
                          dependency.backMap[callee_ttpdecl] = argType;
                        }
                      }
                    }
                    dependencies.push_back(dependency);
                  } else {
                    dependencies.push_back(callee_type);
                  }
                } else {
                  hasUnhandledCandidate = true;
                  break;
                }
                // specializations (C++ only allows full specialization for function templates)
                for (auto specit = callee_ftdecl->spec_begin(); specit != callee_ftdecl->spec_end(); specit++) {
                  auto spec = *specit;
                  if (!(spec->isTemplateInstantiation())) {
                    auto callee_type = spec->getParamDecl(pos)->getType();
                    dependencies.push_back(callee_type);
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
                auto callee_type = callee_fdecl->getParamDecl(pos)->getType();
                dependencies.push_back(callee_type);
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
        } // var used at other places in the callsite
      } else if (auto mexpr = dyn_cast<CXXDependentScopeMemberExpr>(varUseExpr.stmt)) {
        // ignore member accesses via ->
        if (mexpr->isArrow()) {
          continue;
        }
        auto parent = getFirstStmtParent(mexpr, context);
        auto possibleMemberCall = parent ? dyn_cast<CallExpr>(parent) : nullptr;
        // this is a member function
        if (possibleMemberCall && possibleMemberCall->getCallee() == mexpr) {
          bool hasUnhandledArg = false;
          std::vector<TypeBox> parameterTypes;
          for (auto arg = possibleMemberCall->arg_begin(); arg != possibleMemberCall->arg_end(); arg++) {
            auto argType = trySupportedType((*arg)->getType(), tplist, context);
            if (isValidSupportedType(argType)) {
              parameterTypes.push_back(argType);
            } else {
              hasUnhandledArg = true;
              break;
            }
          }
          if (!hasUnhandledArg) {
            TypeBox returnType = simplyInfer(possibleMemberCall, context);
            constraintMap[varUseExpr.ttpdecl].insert(
              MemberConstraint(
                mexpr->getMemberNameInfo().getAsString(),
                true,
                varUseExpr.ttpdecl,
                std::move(parameterTypes),
                returnType
              )
            );
          }
        // this is a member variable
        } else {
          TypeBox returnType = simplyInfer(mexpr, context);
          constraintMap[varUseExpr.ttpdecl].insert(
            MemberConstraint(
              mexpr->getMemberNameInfo().getAsString(),
              false,
              varUseExpr.ttpdecl,
              std::vector<TypeBox>(),
              returnType
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

using AtomicConstraint = std::variant<ConcreteConstraint, UnaryConstraint, BinaryConstraint, FunctionConstraint, MemberConstraint, TypeTraitConstraint>;

struct ConstraintCode {
  ConstraintCode() = default;

  ConstraintCode(const ConstraintCode &c) = delete;

  ConstraintCode &operator= (const ConstraintCode &c) = delete;

  virtual ~ConstraintCode() {}

  virtual bool isValid() {
    return true;
  }

  virtual std::string toStr() {
    return "";
  }
};

struct LiteralConstraintCode : public ConstraintCode {
  LiteralConstraintCode() = default;

  LiteralConstraintCode(const LiteralConstraintCode &c) = delete;

  LiteralConstraintCode &operator= (const LiteralConstraintCode &c) = delete;

  ~LiteralConstraintCode() override {}

  std::string toStr() override {
    return value;
  }

  std::string value;
};

struct ConcreteConstraintCode : public ConstraintCode {
  ConcreteConstraintCode() = default;

  ConcreteConstraintCode(const ConcreteConstraintCode &c) = delete;

  ConcreteConstraintCode &operator= (const ConcreteConstraintCode &c) = delete;

  ~ConcreteConstraintCode() override {}

  bool isValid() override {
    return selfType != "" && targetType != "";
  }

  std::string toStr() override {
    return stringFormat("std::convertible_to<#, #>", {selfType, targetType});
  }

  std::string selfType;
  std::string targetType;
};

struct UnaryConstraintCode : public ConstraintCode {
  UnaryConstraintCode() = default;

  UnaryConstraintCode(const UnaryConstraintCode &c) = delete;

  UnaryConstraintCode &operator= (const UnaryConstraintCode &c) = delete;

  ~UnaryConstraintCode() override {}

  bool isValid() override {
    return selfType != "";
  }

  std::string toStr() override {
    std::string expr = ((position == 0) ? ("x" + operatorName) : (operatorName + "x"));
    return stringFormat("requires (# x) { #; }", {selfType, expr});
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

  bool isValid() override {
    return selfType != "" && otherType != "";
  }

  std::string toStr() override {
    std::string expr = ((position == 0) ? ("x " + operatorName + " y") : ("y " + operatorName + " x"));
    return stringFormat("requires (# x, # y) { #; }", {selfType, otherType, expr});
  }

  std::string selfType;
  std::string operatorName;
  int position;
  std::string otherType;
};

struct FunctionConstraintCode : public ConstraintCode {
  FunctionConstraintCode() = default;

  FunctionConstraintCode(const FunctionConstraintCode &c) = delete;

  FunctionConstraintCode &operator= (const FunctionConstraintCode &c) = delete;

  ~FunctionConstraintCode() override {}

  bool isValid() override {
    if (selfType == "") {
      return false;
    }
    for (const std::string &pt : parameterTypes) {
      if (pt == "") {
        return false;
      }
    }
    return true;
  }

  std::string toStr() override {
    int np = parameterTypes.size();
    std::string pattern = "requires (# f";
    for (int i = 0; i < np; i++) {
      pattern += (", # x" + std::to_string(i));
    }
    pattern += ") { #; }";
    std::string args = "";
    for (int i = 0; i < np; i++) {
      if (args != "") {
        args += ", ";
      }
      args += ("x" + std::to_string(i));
    }
    std::string call = "f(" + args + ")";
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

  std::string selfType;
  std::vector<std::string> parameterTypes;
  std::string returnType;
};

struct MemberConstraintCode : public ConstraintCode {
  MemberConstraintCode() = default;

  MemberConstraintCode(const MemberConstraintCode &c) = delete;

  MemberConstraintCode &operator= (const MemberConstraintCode &c) = delete;

  ~MemberConstraintCode() override {}

  bool isValid() override {
    if (selfType == "") {
      return false;
    }
    for (const std::string &pt : parameterTypes) {
      if (pt == "") {
        return false;
      }
    }
    return true;
  }

  std::string toStr() override {
    int np = parameterTypes.size();
    std::string pattern = "requires (# o";
    for (int i = 0; i < np; i++) {
      pattern += (", # x" + std::to_string(i));
    }
    pattern += ") { #; }";
    std::string access = "o." + memberName;
    if (isFun) {
      access += "(";
      std::string args;
      for (int i = 0; i < np; i++) {
        if (args != "") {
          args += ", ";
        }
        args += ("x" + std::to_string(i));
      }
      access += args;
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

  std::string selfType;
  std::string memberName;
  bool isFun;
  std::vector<std::string> parameterTypes;
  std::string returnType;
};

struct TypeTraitConstraintCode : public ConstraintCode {
  TypeTraitConstraintCode() = default;

  TypeTraitConstraintCode(const TypeTraitConstraintCode &c) = delete;

  TypeTraitConstraintCode &operator= (const TypeTraitConstraintCode &c) = delete;

  ~TypeTraitConstraintCode() override {}

  bool isValid() override {
    return type != "";
  }

  std::string toStr() override {
    return (neg ? "!" : "") + stringFormat("#<#>::value", {trait, type});
  }

  bool neg;
  std::string trait;
  std::string type;
};

struct ConjunctionConstraintCode : public ConstraintCode {
  ConjunctionConstraintCode() = default;

  ConjunctionConstraintCode(const ConjunctionConstraintCode &c) = delete;

  ConjunctionConstraintCode &operator= (const ConjunctionConstraintCode &c) = delete;

  ~ConjunctionConstraintCode() override {
    for (ConstraintCode *c : conjuncts) {
      delete c;
    }
  }

  std::string toStr() override {
    std::string result = "";
    bool multi = false;
    for (ConstraintCode *c : conjuncts) {
      std::string s = c->toStr();
      if (s != "true") {
        if (result != "") {
          result += " && ";
          multi = true;
        }
        result += s;
      }
    }
    if (result == "") {
      return "true";
    } else if (!multi) {
      return result;
    } else {
      return "(" + result + ")";
    }
  }

  std::vector<ConstraintCode*> conjuncts;
};

struct DisjunctionConstraintCode : public ConstraintCode {
  DisjunctionConstraintCode() = default;

  DisjunctionConstraintCode(const DisjunctionConstraintCode &c) = delete;

  DisjunctionConstraintCode &operator= (const DisjunctionConstraintCode &c) = delete;

  ~DisjunctionConstraintCode() override {
    for (ConstraintCode *d : disjuncts) {
      delete d;
    }
  }

  std::string toStr() override {
    std::string result = "";
    bool multi = false;
    for (ConstraintCode *d : disjuncts) {
      std::string s = d->toStr();
      if (s != "false") {
        if (result != "") {
          result += " || ";
          multi = true;
        }
        result += s;
      }
    }
    if (result == "") {
      return "false";
    } else if (!multi) {
      return result;
    } else {
      return "(" + result + ")";
    }
  }

  std::vector<ConstraintCode*> disjuncts;
};

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
    return false;
  }

  virtual ConstraintCode *printConstraintCode(std::vector<const BackMap*> &backMaps) const {
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

  ConstraintCode *printConstraintCode(std::vector<const BackMap*> &backMaps) const override {
    auto l = new LiteralConstraintCode();
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

  ConstraintCode *printConstraintCode(std::vector<const BackMap*> &backMaps) const override {
    auto resolveType = [&](const TypeBox &t) -> std::string {
      if (std::holds_alternative<std::string>(t)) {
        return std::get<std::string>(t);
      } else {
        auto st = std::get<SupportedType>(t);
        if (std::holds_alternative<QualType>(st)) {
          return std::get<QualType>(st).getAsString();
        } else {
          auto ttpdecl = std::get<const TemplateTypeParmDecl*>(st);
          for (auto backMapPtr = backMaps.rbegin(); backMapPtr != backMaps.rend(); backMapPtr++) {
            if ((**backMapPtr).count(ttpdecl) == 0) {
              return "";
            } else {
              auto nx = (**backMapPtr).at(ttpdecl);
              if (std::holds_alternative<QualType>(nx)) {
                return std::get<QualType>(nx).getAsString();
              } else {
                ttpdecl = std::get<const TemplateTypeParmDecl*>(nx);
              }
            }
          }
          return ttpdecl->getNameAsString();
        }
      }
    };
#define CHECK_RETURN(c) do {\
  if ((c)->isValid()) {\
    return (c);\
  } else {\
    delete (c);\
    auto t = new LiteralConstraintCode();\
    t->value = "true";\
    return t;\
  }\
} while (0)
    if (std::holds_alternative<ConcreteConstraint>(con)) {
      ConcreteConstraint c = std::get<ConcreteConstraint>(con);
      auto ccc = new ConcreteConstraintCode();
      ccc->selfType = resolveType(c.selfType);
      ccc->targetType = resolveType(c.constraintType);
      CHECK_RETURN(ccc);
    } else if (std::holds_alternative<UnaryConstraint>(con)) {
      UnaryConstraint u = std::get<UnaryConstraint>(con);
      auto ucc = new UnaryConstraintCode();
      ucc->operatorName = u.op;
      ucc->position = u.pos;
      ucc->selfType = resolveType(u.selfType);
      CHECK_RETURN(ucc);
    } else if (std::holds_alternative<BinaryConstraint>(con)) {
      BinaryConstraint b = std::get<BinaryConstraint>(con);
      auto bcc = new BinaryConstraintCode();
      bcc->operatorName = b.op;
      bcc->position = b.pos;
      bcc->selfType = resolveType(b.selfType);
      bcc->otherType = resolveType(b.otherType);
      CHECK_RETURN(bcc);
    } else if (std::holds_alternative<FunctionConstraint>(con)) {
      FunctionConstraint f = std::get<FunctionConstraint>(con);
      auto fcc = new FunctionConstraintCode();
      fcc->selfType = resolveType(f.selfType);
      if (std::holds_alternative<std::string>(f.parameterTypes)) {
        CHECK_RETURN(fcc);
      }
      for (const auto &pt : std::get<std::vector<TypeBox>>(f.parameterTypes)) {
        fcc->parameterTypes.push_back(resolveType(pt));
      }
      fcc->returnType = resolveType(f.returnType);
      CHECK_RETURN(fcc);
    } else if (std::holds_alternative<MemberConstraint>(con)) {
      MemberConstraint m = std::get<MemberConstraint>(con);
      auto mcc = new MemberConstraintCode();
      mcc->memberName = m.mb;
      mcc->isFun = m.isFun;
      mcc->selfType = resolveType(m.selfType);
      if (std::holds_alternative<std::string>(m.parameterTypes)) {
        CHECK_RETURN(mcc);
      }
      for (const auto &pt : std::get<std::vector<TypeBox>>(m.parameterTypes)) {
        mcc->parameterTypes.push_back(resolveType(pt));
      }
      mcc->returnType = resolveType(m.returnType);
      CHECK_RETURN(mcc);
    } else if (std::holds_alternative<TypeTraitConstraint>(con)) {
      TypeTraitConstraint t = std::get<TypeTraitConstraint>(con);
      auto tcc = new TypeTraitConstraintCode();
      tcc->neg = t.neg;
      tcc->trait = t.predicate;
      tcc->type = resolveType(t.ttpdecl);
      CHECK_RETURN(tcc);
    } else {
      auto dummy = new ConstraintCode();
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

  ~Conjunction() override {
    // We don't release conjuncts' memory here. Instead, we use a memory pool.
  }

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

  ConstraintCode *printConstraintCode(std::vector<const BackMap*> &backMaps) const override {
    auto ccc = new ConjunctionConstraintCode();
    for (auto f : conjuncts) {
      ccc->conjuncts.push_back(f->printConstraintCode(backMaps));
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

  ~Disjunction() override {
    // We don't release disjuncts' memory here. Instead, we use a memory pool.
  }

  std::string toStr() const override {
    std::string content;
    for (const std::pair<Formula*, BackMap> &p : disjuncts) {
      if (content != "") {
        content += " ";
      }
      content += p.first->toStr();
      content += " ";
      content += backMapToString(p.second);
    }
    return CLASS_STRING(content);
  }

  int literalStatus() const override {
    return -1;
  }

  bool evaluate(const std::function<bool(const AtomicConstraint&)> &has_constraint) const override {
    bool ret = false;
    for (const std::pair<Formula*, BackMap> &p : disjuncts) {
      ret = ret || ((p.first)->evaluate(has_constraint));
    }
    return ret;
  }

  ConstraintCode *printConstraintCode(std::vector<const BackMap*> &backMaps) const override {
    auto dcc = new DisjunctionConstraintCode();
    for (const std::pair<Formula*, BackMap> &p : disjuncts) {
      if (!(p.second.empty())) {
        backMaps.push_back(&(p.second));
      }
      dcc->disjuncts.push_back((p.first)->printConstraintCode(backMaps));
      if (!(p.second.empty())) {
        backMaps.pop_back();
      }
    }
    return dcc;
  }

  void addDisjunct(Formula *f, BackMap backMap = {}) {
    disjuncts.push_back(std::make_pair(f, std::move(backMap)));
  }

  std::vector<std::pair<Formula*, BackMap>> disjuncts;
};

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
      if (std::holds_alternative<ConcreteConstraint>(c)) {
        return false;
      } else if (std::holds_alternative<UnaryConstraint>(c)) {
        return match<UnaryConstraint>(unaryPatterns, std::get<UnaryConstraint>(c));
      } else if (std::holds_alternative<BinaryConstraint>(c)) {
        return match<BinaryConstraint>(binaryPatterns, std::get<BinaryConstraint>(c));
      } else if (std::holds_alternative<FunctionConstraint>(c)) {
        return match<FunctionConstraint>(functionPatterns, std::get<FunctionConstraint>(c));
      } else if (std::holds_alternative<MemberConstraint>(c)) {
        return match<MemberConstraint>(memberPatterns, std::get<MemberConstraint>(c));
      } else { // TypeTraitConstraint
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
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("=")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "InputIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", "", 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "OutputIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("="),
          UnaryConstraint("++", "", 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "ForwardIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", "", 0)
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "BidirectionalIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", "", 0),
          UnaryConstraint("--")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "RandomAccessIterator",
      {
        ConstraintPredicate({
          UnaryConstraint("*", "", 1),
          UnaryConstraint("++", "", 1),
          BinaryConstraint("="),
          BinaryConstraint("=="),
          BinaryConstraint("!="),
          UnaryConstraint("++", "", 0),
          UnaryConstraint("--"),
          BinaryConstraint("+"),
          BinaryConstraint("-"),
          BinaryConstraint("<"),
          BinaryConstraint(">"),
          BinaryConstraint("<="),
          BinaryConstraint(">=")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "Callable",
      {
        ConstraintPredicate({
          FunctionConstraint()
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "Predicate",
      {
        ConstraintPredicate({
          FunctionConstraint("", std::vector<TypeBox>{""}, "bool")
        }),
        [](const Instantiation &i){ return true; }
      }
    },
    {
      "BinaryPredicate",
      {
        ConstraintPredicate({
          FunctionConstraint("", std::vector<TypeBox>{"", ""}, "bool")
        }),
        [](const Instantiation &i){ return true; }
      }
    }
  };

}

// Later may add support of two or more named requirements.
std::vector<std::string> infer(
  const Formula *formula,
  const std::unordered_set<Instantiation> &instantiation_set) {
  std::vector<std::string> requirements;
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
        for (const auto &con : visitor.constraintMap.at(ttpd)) {
          if (std::holds_alternative<UnaryConstraint>(con)) {
            auto u = pool.poolNew<Atomic>(std::get<UnaryConstraint>(con));
            conj->addConjunct(u);
          } else if (std::holds_alternative<BinaryConstraint>(con)) {
            auto b = pool.poolNew<Atomic>(std::get<BinaryConstraint>(con));
            conj->addConjunct(b);
          } else if (std::holds_alternative<MemberConstraint>(con)) {
            auto m = pool.poolNew<Atomic>(std::get<MemberConstraint>(con));
            conj->addConjunct(m);
          } else if (std::holds_alternative<FunctionConstraint>(con)) {
            auto f = pool.poolNew<Atomic>(std::get<FunctionConstraint>(con));
            conj->addConjunct(f);
          } else if (std::holds_alternative<TypeTraitConstraint>(con)) {
            auto t = pool.poolNew<Atomic>(std::get<TypeTraitConstraint>(con));
            conj->addConjunct(t);
          } else if (std::holds_alternative<CallConstraint>(con)) {
            auto c = std::get<CallConstraint>(con);
            auto disj = pool.poolNew<Disjunction>();
            for (const auto &cc : c.dependencies) {
              if (std::holds_alternative<TypeBox>(cc)) {
                auto a = pool.poolNew<Atomic>(ConcreteConstraint(c.selfType, std::get<TypeBox>(cc)));
                disj->addDisjunct(a);
              } else if (std::holds_alternative<TemplateDependency>(cc)) {
                const auto &cte = std::get<TemplateDependency>(cc);
                if (auto f = dfs(cte.ttpdecl)) {
                  disj->addDisjunct(f, cte.backMap);
                } else { // recursive dependency
                  auto t = pool.poolNew<Literal>(true);
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

    std::vector<std::string> prints;
    for (const auto &kv : results) {
      std::ostringstream os;
      auto ttpdecl = kv.first;
      auto f = kv.second;
      auto ftdecl = fromTemplateTypeParmDeclToFunctionTemplateDecl(ttpdecl, &context);
      if (ftdecl) {
        os << "[" << ftdecl->getNameAsString() << ":" << ftdecl->getAsFunction()->getNumParams() << ", " << ttpdecl->getNameAsString() << "]\n";
        std::vector<const BackMap*> backMaps;
        auto cc = f->printConstraintCode(backMaps);
        os << "\tPrinted code: " << cc->toStr() << '\n';
        delete cc;
        os << "\tInferred constraint:";
        const auto &inferred = infer(f, visitor.instantiationMap[ttpdecl]);
        for (const auto &con : inferred) {
          os << " " << con;
        }
        os << "\n";
      }
      auto s = os.str();
      if (s.size() > 0) {
        prints.push_back(s);
      }
    }
    std::sort(prints.begin(), prints.end());
    for (const auto &s : prints) {
      llvm::outs() << s;
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
