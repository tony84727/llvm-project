//===--- ExtractVariable.cpp ------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "ClangdUnit.h"
#include "Logger.h"
#include "Protocol.h"
#include "Selection.h"
#include "SourceCode.h"
#include "refactor/Tweak.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace clangd {
namespace {
// information regarding the Expr that is being extracted
class ExtractionContext {
public:
  ExtractionContext(const SelectionTree::Node *Node, const SourceManager &SM,
                    const ASTContext &Ctx);
  const clang::Expr *getExpr() const { return Expr; }
  const SelectionTree::Node *getExprNode() const { return ExprNode; }
  bool isExtractable() const { return Extractable; }
  // Generate Replacement for replacing selected expression with given VarName
  tooling::Replacement replaceWithVar(llvm::StringRef VarName) const;
  // Generate Replacement for declaring the selected Expr as a new variable
  tooling::Replacement insertDeclaration(llvm::StringRef VarName) const;

private:
  bool Extractable = false;
  const clang::Expr *Expr;
  const SelectionTree::Node *ExprNode;
  // Stmt before which we will extract
  const clang::Stmt *InsertionPoint = nullptr;
  const SourceManager &SM;
  const ASTContext &Ctx;
  // Decls referenced in the Expr
  std::vector<clang::Decl *> ReferencedDecls;
  // returns true if the Expr doesn't reference any variable declared in scope
  bool exprIsValidOutside(const clang::Stmt *Scope) const;
  // computes the Stmt before which we will extract out Expr
  const clang::Stmt *computeInsertionPoint() const;
};

// Returns all the Decls referenced inside the given Expr
static std::vector<clang::Decl *>
computeReferencedDecls(const clang::Expr *Expr) {
  // RAV subclass to find all DeclRefs in a given Stmt
  class FindDeclRefsVisitor
      : public clang::RecursiveASTVisitor<FindDeclRefsVisitor> {
  public:
    std::vector<Decl *> ReferencedDecls;
    bool VisitDeclRefExpr(DeclRefExpr *DeclRef) { // NOLINT
      ReferencedDecls.push_back(DeclRef->getDecl());
      return true;
    }
  };
  FindDeclRefsVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Stmt *>(dyn_cast<Stmt>(Expr)));
  return Visitor.ReferencedDecls;
}

ExtractionContext::ExtractionContext(const SelectionTree::Node *Node,
                                     const SourceManager &SM,
                                     const ASTContext &Ctx)
    : ExprNode(Node), SM(SM), Ctx(Ctx) {
  Expr = Node->ASTNode.get<clang::Expr>();
  ReferencedDecls = computeReferencedDecls(Expr);
  InsertionPoint = computeInsertionPoint();
  if (InsertionPoint)
    Extractable = true;
}

// checks whether extracting before InsertionPoint will take a
// variable reference out of scope
bool ExtractionContext::exprIsValidOutside(const clang::Stmt *Scope) const {
  SourceLocation ScopeBegin = Scope->getBeginLoc();
  SourceLocation ScopeEnd = Scope->getEndLoc();
  for (const Decl *ReferencedDecl : ReferencedDecls) {
    if (SM.isPointWithin(ReferencedDecl->getBeginLoc(), ScopeBegin, ScopeEnd) &&
        SM.isPointWithin(ReferencedDecl->getEndLoc(), ScopeBegin, ScopeEnd))
      return false;
  }
  return true;
}

// Return the Stmt before which we need to insert the extraction.
// To find the Stmt, we go up the AST Tree and if the Parent of the current
// Stmt is a CompoundStmt, we can extract inside this CompoundStmt just before
// the current Stmt. We ALWAYS insert before a Stmt whose parent is a
// CompoundStmt
//
// FIXME: Extraction from label, switch and case statements
// FIXME: Doens't work for FoldExpr
// FIXME: Ensure extraction from loops doesn't change semantics.
const clang::Stmt *ExtractionContext::computeInsertionPoint() const {
  // returns true if we can extract before InsertionPoint
  auto CanExtractOutside =
      [](const SelectionTree::Node *InsertionPoint) -> bool {
    if (const clang::Stmt *Stmt = InsertionPoint->ASTNode.get<clang::Stmt>()) {
      // Allow all expressions except LambdaExpr since we don't want to extract
      // from the captures/default arguments of a lambda
      if (isa<clang::Expr>(Stmt))
        return !isa<LambdaExpr>(Stmt);
      // We don't yet allow extraction from switch/case stmt as we would need to
      // jump over the switch stmt even if there is a CompoundStmt inside the
      // switch. And there are other Stmts which we don't care about (e.g.
      // continue and break) as there can never be anything to extract from
      // them.
      return isa<AttributedStmt>(Stmt) || isa<CompoundStmt>(Stmt) ||
             isa<CXXForRangeStmt>(Stmt) || isa<DeclStmt>(Stmt) ||
             isa<DoStmt>(Stmt) || isa<ForStmt>(Stmt) || isa<IfStmt>(Stmt) ||
             isa<ReturnStmt>(Stmt) || isa<WhileStmt>(Stmt);
    }
    if (InsertionPoint->ASTNode.get<VarDecl>())
      return true;
    return false;
  };
  for (const SelectionTree::Node *CurNode = getExprNode();
       CurNode->Parent && CanExtractOutside(CurNode);
       CurNode = CurNode->Parent) {
    const clang::Stmt *CurInsertionPoint = CurNode->ASTNode.get<Stmt>();
    // give up if extraction will take a variable out of scope
    if (CurInsertionPoint && !exprIsValidOutside(CurInsertionPoint))
      break;
    if (const clang::Stmt *CurParent = CurNode->Parent->ASTNode.get<Stmt>()) {
      if (isa<CompoundStmt>(CurParent)) {
        // Ensure we don't write inside a macro.
        if (CurParent->getBeginLoc().isMacroID())
          continue;
        return CurInsertionPoint;
      }
    }
  }
  return nullptr;
}
// returns the replacement for substituting the extraction with VarName
tooling::Replacement
ExtractionContext::replaceWithVar(llvm::StringRef VarName) const {
  const llvm::Optional<SourceRange> ExtractionRng =
      toHalfOpenFileRange(SM, Ctx.getLangOpts(), getExpr()->getSourceRange());
  unsigned ExtractionLength = SM.getFileOffset(ExtractionRng->getEnd()) -
                              SM.getFileOffset(ExtractionRng->getBegin());
  return tooling::Replacement(SM, ExtractionRng->getBegin(), ExtractionLength,
                              VarName);
}
// returns the Replacement for declaring a new variable storing the extraction
tooling::Replacement
ExtractionContext::insertDeclaration(llvm::StringRef VarName) const {
  const llvm::Optional<SourceRange> ExtractionRng =
      toHalfOpenFileRange(SM, Ctx.getLangOpts(), getExpr()->getSourceRange());
  assert(ExtractionRng && "ExtractionRng should not be null");
  llvm::StringRef ExtractionCode = toSourceCode(SM, *ExtractionRng);
  const SourceLocation InsertionLoc =
      toHalfOpenFileRange(SM, Ctx.getLangOpts(),
                          InsertionPoint->getSourceRange())
          ->getBegin();
  // FIXME: Replace auto with explicit type and add &/&& as necessary
  std::string ExtractedVarDecl = std::string("auto ") + VarName.str() + " = " +
                                 ExtractionCode.str() + "; ";
  return tooling::Replacement(SM, InsertionLoc, 0, ExtractedVarDecl);
}

/// Extracts an expression to the variable dummy
/// Before:
/// int x = 5 + 4 * 3;
///         ^^^^^
/// After:
/// auto dummy = 5 + 4;
/// int x = dummy * 3;
class ExtractVariable : public Tweak {
public:
  const char *id() const override final;
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override {
    return "Extract subexpression to variable";
  }
  Intent intent() const override { return Refactor; }
  // Compute the extraction context for the Selection
  bool computeExtractionContext(const SelectionTree::Node *N,
                                const SourceManager &SM, const ASTContext &Ctx);

private:
  // the expression to extract
  std::unique_ptr<ExtractionContext> Target;
};
REGISTER_TWEAK(ExtractVariable)
bool ExtractVariable::prepare(const Selection &Inputs) {
  // we don't trigger on empty selections for now
  if (Inputs.SelectionBegin == Inputs.SelectionEnd)
    return false;
  const ASTContext &Ctx = Inputs.AST.getASTContext();
  const SourceManager &SM = Inputs.AST.getSourceManager();
  const SelectionTree::Node *N = Inputs.ASTSelection.commonAncestor();
  return computeExtractionContext(N, SM, Ctx);
}

Expected<Tweak::Effect> ExtractVariable::apply(const Selection &Inputs) {
  tooling::Replacements Result;
  // FIXME: get variable name from user or suggest based on type
  std::string VarName = "dummy";
  // insert new variable declaration
  if (auto Err = Result.add(Target->insertDeclaration(VarName)))
    return std::move(Err);
  // replace expression with variable name
  if (auto Err = Result.add(Target->replaceWithVar(VarName)))
    return std::move(Err);
  return Effect::applyEdit(Result);
}

// Find the CallExpr whose callee is an ancestor of the DeclRef
const SelectionTree::Node *getCallExpr(const SelectionTree::Node *DeclRef) {
  // we maintain a stack of all exprs encountered while traversing the
  // selectiontree because the callee of the callexpr can be an ancestor of the
  // DeclRef. e.g. Callee can be an ImplicitCastExpr.
  std::vector<const clang::Expr *> ExprStack;
  for (auto *CurNode = DeclRef; CurNode; CurNode = CurNode->Parent) {
    const Expr *CurExpr = CurNode->ASTNode.get<Expr>();
    if (const CallExpr *CallPar = CurNode->ASTNode.get<CallExpr>()) {
      // check whether the callee of the callexpr is present in Expr stack.
      if (std::find(ExprStack.begin(), ExprStack.end(), CallPar->getCallee()) !=
          ExprStack.end())
        return CurNode;
      return nullptr;
    }
    ExprStack.push_back(CurExpr);
  }
  return nullptr;
}

// check if Expr can be assigned to a variable i.e. is non-void type
bool canBeAssigned(const SelectionTree::Node *ExprNode) {
  const clang::Expr *Expr = ExprNode->ASTNode.get<clang::Expr>();
  if (const Type *ExprType = Expr->getType().getTypePtrOrNull())
    // FIXME: check if we need to cover any other types
    return !ExprType->isVoidType();
  return true;
}

// Find the node that will form our ExtractionContext.
// We don't want to trigger for assignment expressions and variable/field
// DeclRefs. For function/member function, we want to extract the entire
// function call.
bool ExtractVariable::computeExtractionContext(const SelectionTree::Node *N,
                                               const SourceManager &SM,
                                               const ASTContext &Ctx) {
  if (!N)
    return false;
  const clang::Expr *SelectedExpr = N->ASTNode.get<clang::Expr>();
  const SelectionTree::Node *TargetNode = N;
  if (!SelectedExpr)
    return false;
  // Extracting Exprs like a = 1 gives dummy = a = 1 which isn't useful.
  if (const BinaryOperator *BinOpExpr =
          dyn_cast_or_null<BinaryOperator>(SelectedExpr)) {
    if (BinOpExpr->getOpcode() == BinaryOperatorKind::BO_Assign)
      return false;
  }
  // For function and member function DeclRefs, we look for a parent that is a
  // CallExpr
  if (const DeclRefExpr *DeclRef =
          dyn_cast_or_null<DeclRefExpr>(SelectedExpr)) {
    // Extracting just a variable isn't that useful.
    if (!isa<FunctionDecl>(DeclRef->getDecl()))
      return false;
    TargetNode = getCallExpr(N);
  }
  if (const MemberExpr *Member = dyn_cast_or_null<MemberExpr>(SelectedExpr)) {
    // Extracting just a field member isn't that useful.
    if (!isa<CXXMethodDecl>(Member->getMemberDecl()))
      return false;
    TargetNode = getCallExpr(N);
  }
  if (!TargetNode || !canBeAssigned(TargetNode))
    return false;
  Target = llvm::make_unique<ExtractionContext>(TargetNode, SM, Ctx);
  return Target->isExtractable();
}

} // namespace
} // namespace clangd
} // namespace clang
