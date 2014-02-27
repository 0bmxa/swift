//===--- TypeChecker.cpp - Type Checking ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the swift::performTypeChecking entry point for
// semantic analysis.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

TypeChecker::TypeChecker(ASTContext &Ctx, DiagnosticEngine &Diags)
  : Context(Ctx), Diags(Diags)
{
  Context.addMutationListener(*this);

  // Add any external definitions already part of the module.
  // Note: the underlying vector can get reallocated during this traversal.
  // We don't want to pick up the new external definitions.
  unsigned n = Context.ExternalDefinitions.size();
  for (unsigned i = Context.LastCheckedExternalDefinition; i != n; ++i)
    handleExternalDecl(Context.ExternalDefinitions[i]);
}

TypeChecker::~TypeChecker() {
  Context.removeMutationListener(*this);
}

void TypeChecker::handleExternalDecl(Decl *decl) {
  if (auto SD = dyn_cast<StructDecl>(decl)) {
    addImplicitConstructors(SD);
  }
  if (auto CD = dyn_cast<ClassDecl>(decl)) {
    addImplicitDestructor(CD);
  }
  if (auto ED = dyn_cast<EnumDecl>(decl)) {
    addImplicitEnumConformances(ED);
  }
}

void TypeChecker::addedExternalDecl(Decl *decl) {
  handleExternalDecl(decl);
}

ProtocolDecl *TypeChecker::getProtocol(SourceLoc loc, KnownProtocolKind kind) {
  auto protocol = Context.getProtocol(kind);
  if (!protocol && loc.isValid()) {
    diagnose(loc, diag::missing_protocol,
             Context.getIdentifier(getProtocolName(kind)));
  }

  if (protocol && !protocol->hasType()) {
    validateDecl(protocol);
    if (protocol->isInvalid())
      return nullptr;
  }

  return protocol;
}

ProtocolDecl *TypeChecker::getLiteralProtocol(Expr *expr) {
  if (isa<ArrayExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::ArrayLiteralConvertible);

  if (isa<DictionaryExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::DictionaryLiteralConvertible);

  if (!isa<LiteralExpr>(expr))
    return nullptr;
  
  if (isa<IntegerLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::IntegerLiteralConvertible);

  if (isa<FloatLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::FloatLiteralConvertible);

  if (isa<CharacterLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::CharacterLiteralConvertible);

  if (isa<StringLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::StringLiteralConvertible);

  if (isa<InterpolatedStringLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::StringInterpolationConvertible);

  if (auto E = dyn_cast<MagicIdentifierLiteralExpr>(expr)) {
    switch (E->getKind()) {
    case MagicIdentifierLiteralExpr::File:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringLiteralConvertible);

    case MagicIdentifierLiteralExpr::Line:
    case MagicIdentifierLiteralExpr::Column:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::IntegerLiteralConvertible);
    }
  }
  
  return nullptr;
}

Module *TypeChecker::getStdlibModule(const DeclContext *dc) {
  if (StdlibModule)
    return StdlibModule;

  if (!StdlibModule)
    StdlibModule = Context.getStdlibModule();
  if (!StdlibModule)
    StdlibModule = dc->getParentModule();

  assert(StdlibModule && "no main module found");
  Context.recordKnownProtocols(StdlibModule);
  return StdlibModule;
}

Type TypeChecker::lookupBoolType(const DeclContext *dc) {
  return boolType.cache([&]{
    UnqualifiedLookup boolLookup(Context.getIdentifier("Bool"),
                                 getStdlibModule(dc), nullptr,
                                 SourceLoc(),
                                 /*IsTypeLookup=*/true);
    if (!boolLookup.isSuccess()) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    TypeDecl *tyDecl = boolLookup.getSingleTypeResult();
    
    if (!tyDecl) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    
    return tyDecl->getDeclaredType();
  });
}

static void bindExtensionDecl(ExtensionDecl *ED, TypeChecker &TC) {
  if (ED->getExtendedTypeLoc().wasValidated())
    return;
  
  // FIXME: Should require bound generics here.
  if (TC.validateType(ED->getExtendedTypeLoc(), ED->getDeclContext(),
                      TR_AllowUnboundGenerics)) {
    ED->setInvalid();
    return;
  }

  Type ExtendedTy = ED->getExtendedType()->getCanonicalType();
  if (!ExtendedTy->is<NominalType>() && !ExtendedTy->is<UnboundGenericType>() &&
      !ExtendedTy->is<ErrorType>()) {
    TC.diagnose(ED, diag::non_nominal_extension,
                false, ED->getExtendedType());
    ED->getExtendedTypeLoc().setInvalidType(TC.Context);
    return;
  }

  if (auto nominal = ExtendedTy->getAnyNominal())
    nominal->addExtension(ED);
}

/// Returns true if the given decl or extension conforms to a protocol whose
/// name matches a compiler-known protocol. This is a syntactic check; no type
/// resolution is performed.
template <typename DeclTy>
static bool mayConformToKnownProtocol(const DeclTy *D) {
  for (TypeLoc inherited : D->getInherited()) {
    auto identRepr = dyn_cast_or_null<IdentTypeRepr>(inherited.getTypeRepr());
    if (!identRepr)
      continue;

    auto lastSimpleID =
        dyn_cast<SimpleIdentTypeRepr>(identRepr->getComponentRange().back());
    if (!lastSimpleID)
      continue;

    bool matchesKnownProtocol =
      llvm::StringSwitch<bool>(lastSimpleID->getIdentifier().str())
#define PROTOCOL(Name) \
        .Case(#Name, true)
#include "swift/AST/KnownProtocols.def"
        .Default(false);
    if (matchesKnownProtocol)
      return true;
  }

  return false;
}

static void checkBridgingFunctions(TypeChecker &tc, Module *mod,
                                   StringRef bridgedTypeName,
                                   StringRef forwardConversion,
                                   StringRef reverseConversion) {
  assert(mod);
  Module::AccessPathTy unscopedAccess = {};
  SmallVector<ValueDecl *, 4> results;

  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(bridgedTypeName),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(forwardConversion),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(reverseConversion),
                   NLKind::QualifiedLookup, results);

  for (auto D : results)
    tc.validateDecl(D);
}

static bool haveDifferentFixity(const ValueDecl *lhs, const ValueDecl *rhs) {
  auto lhsFn = dyn_cast<FuncDecl>(lhs);
  auto rhsFn = dyn_cast<FuncDecl>(rhs);
  if (!lhsFn || !rhsFn)
    return false;

  assert(lhs->getName() == rhs->getName());
  if (!lhs->getName().isOperator())
    return false;

  assert(lhs->getAttrs().isPrefix() ^ lhs->getAttrs().isPostfix());
  assert(rhs->getAttrs().isPrefix() ^ rhs->getAttrs().isPostfix());
  return lhs->getAttrs().isPrefix() != rhs->getAttrs().isPrefix();
}

void swift::performTypeChecking(SourceFile &SF, TopLevelContext &TLC,
                                unsigned StartElem) {
  if (SF.ASTStage == SourceFile::TypeChecked)
    return;

  // Make sure that name binding has been completed before doing any type
  // checking.
  performNameBinding(SF, StartElem);

  TypeChecker TC(SF.getASTContext());
  auto &DefinedFunctions = TC.definedFunctions;
  size_t existingExternalDecls = TC.Context.ExternalDefinitions.size();
  
  // Lookup the swift module.  This ensures that we record all known protocols
  // in the AST.
  (void) TC.getStdlibModule(&SF);

  // Resolve extensions. This has to occur first during type checking,
  // because the extensions need to be wired into the AST for name lookup
  // to work.
  // FIXME: We can have interesting ordering dependencies among the various
  // extensions, so we'll need to be smarter here.
  // FIXME: The current source file needs to be handled specially, because of
  // private extensions.
  SF.forAllVisibleModules([&](Module::ImportedModule import) {
    // FIXME: Respect the access path?
    for (auto file : import.second->getFiles()) {
      auto SF = dyn_cast<SourceFile>(file);
      if (!SF)
        continue;

      for (auto D : SF->Decls) {
        if (auto ED = dyn_cast<ExtensionDecl>(D)) {
          bindExtensionDecl(ED, TC);
          if (mayConformToKnownProtocol(ED))
            TC.validateDecl(ED->getExtendedType()->getAnyNominal());
        } else if (auto nominal = dyn_cast<NominalTypeDecl>(D)) {
          if (mayConformToKnownProtocol(nominal))
            TC.validateDecl(nominal);
        }
      }
    }
  });

  // FIXME: Check for cycles in class inheritance here?

  // Type check the top-level elements of the source file.
  for (auto D : llvm::makeArrayRef(SF.Decls).slice(StartElem)) {
    if (isa<TopLevelCodeDecl>(D))
      continue;

    TC.typeCheckDecl(D, /*isFirstPass*/true);
  }

  // At this point, we can perform general name lookup into any type.

  // We don't know the types of all the global declarations in the first
  // pass, which means we can't completely analyze everything. Perform the
  // second pass now.

  bool hasTopLevelCode = false;
  for (auto D : llvm::makeArrayRef(SF.Decls).slice(StartElem)) {
    if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      hasTopLevelCode = true;
      // Immediately perform global name-binding etc.
      TC.typeCheckTopLevelCodeDecl(TLCD);
    } else {
      TC.typeCheckDecl(D, /*isFirstPass*/false);
    }
  }

  if (hasTopLevelCode) {
    TC.contextualizeTopLevelCode(TLC,
                           llvm::makeArrayRef(SF.Decls).slice(StartElem));
  }

#define BRIDGE_TYPE(BRIDGED_MOD, BRIDGED_TYPE, _, NATIVE_TYPE) \
  if (Module *module = SF.getASTContext().LoadedModules.lookup(#BRIDGED_MOD)) {\
    checkBridgingFunctions(TC, module, #BRIDGED_TYPE, \
                           "convert" #BRIDGED_TYPE "To" #NATIVE_TYPE, \
                           "convert" #NATIVE_TYPE "To" #BRIDGED_TYPE); \
  }
#include "swift/SIL/BridgedTypes.def"

  // Define any pending implicitly declarations.
  TC.definePendingImplicitDecls();
  DefinedFunctions.insert(DefinedFunctions.end(),
                          TC.implicitlyDefinedFunctions.begin(),
                          TC.implicitlyDefinedFunctions.end());
  TC.implicitlyDefinedFunctions.clear();

  // If we're in REPL mode, inject temporary result variables and other stuff
  // that the REPL needs to synthesize.
  if (SF.Kind == SourceFileKind::REPL && !TC.Context.hadError())
    TC.processREPLTopLevel(SF, TLC, StartElem);

  // Check overloaded vars/funcs.
  // FIXME: This is quadratic time for source files with multiple chunks.
  // FIXME: Can we make this more efficient?
  // FIXME: This check should be earlier to avoid ambiguous overload
  // errors etc.
  llvm::DenseMap<Identifier, TinyPtrVector<ValueDecl*>> CheckOverloads;
  for (unsigned i = 0, e = SF.Decls.size(); i != e; ++i) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(SF.Decls[i])) {
      // FIXME: I'm not sure this check is really correct.
      if (!VD->hasName())
        continue;
      if (VD->getType()->is<ErrorType>())
        continue;
      auto &PrevOv = CheckOverloads[VD->getName()];
      if (i >= StartElem) {
        for (ValueDecl *PrevD : PrevOv) {
          if (PrevD->getType()->isEqual(VD->getType()) &&
              !haveDifferentFixity(PrevD, VD)) {
            TC.diagnose(VD->getStartLoc(), diag::invalid_redecl);
            TC.diagnose(PrevD, diag::invalid_redecl_prev,
                        VD->getName());
          }
        }
      }
      PrevOv.push_back(VD);
    }
  }

  unsigned currentFunctionIdx = 0;
  unsigned currentExternalDef = TC.Context.LastCheckedExternalDefinition;
  do {
    // Type check the body of each of the function in turn.  Note that outside
    // functions must be visited before nested functions for type-checking to
    // work correctly.
    unsigned previousFunctionIdx = currentFunctionIdx;
    for (unsigned n = DefinedFunctions.size(); currentFunctionIdx != n;
         ++currentFunctionIdx) {
      auto *AFD = DefinedFunctions[currentFunctionIdx];
      PrettyStackTraceDecl StackEntry("type-checking", AFD);
      TC.typeCheckAbstractFunctionBody(AFD);
    }

    // Compute captures for functions we visited, in the opposite order of type
    // checking. i.e., the nested DefinedFunctions will be visited before the
    // outer DefinedFunctions.
    for (unsigned i = currentFunctionIdx; i > previousFunctionIdx; --i) {
      if (auto *FD = dyn_cast<AbstractFunctionDecl>(DefinedFunctions[i-1]))
        TC.computeCaptures(FD);
    }

    for (unsigned n = TC.Context.ExternalDefinitions.size();
         currentExternalDef != n;
         ++currentExternalDef) {
      auto decl = TC.Context.ExternalDefinitions[currentExternalDef];

      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl)) {
        PrettyStackTraceDecl StackEntry("type-checking", AFD);
        TC.typeCheckAbstractFunctionBody(AFD);
        continue;
      }
       if (isa<StructDecl>(decl) || isa<ClassDecl>(decl) ||
           isa<EnumDecl>(decl) || isa<ProtocolDecl>(decl)) {
         if (currentExternalDef < existingExternalDecls)
           TC.handleExternalDecl(decl);
         continue;
      }
      llvm_unreachable("Unhandled external definition kind");
    }

    // Type-check any referenced nominal types.
    while (!TC.ValidatedTypes.empty()) {
      auto nominal = TC.ValidatedTypes.back();
      TC.ValidatedTypes.pop_back();
      TC.typeCheckDecl(nominal, /*isFirstPass=*/true);
    }

    // Define any pending implicit declarations.
    TC.definePendingImplicitDecls();
    DefinedFunctions.insert(DefinedFunctions.end(),
                            TC.implicitlyDefinedFunctions.begin(),
                            TC.implicitlyDefinedFunctions.end());
    TC.implicitlyDefinedFunctions.clear();

  } while (currentFunctionIdx < DefinedFunctions.size() ||
           currentExternalDef < TC.Context.ExternalDefinitions.size());

  // FIXME: Horrible hack. Store this somewhere more sane.
  TC.Context.LastCheckedExternalDefinition = currentExternalDef;

  // Verify that we've checked types correctly.
  SF.ASTStage = SourceFile::TypeChecked;
  verify(SF);

  // Verify modules imported by Clang importer.
#ifndef NDEBUG
  if (SF.Kind != SourceFileKind::REPL)
    if (auto ClangLoader = TC.Context.getClangModuleLoader())
      ClangLoader->verifyAllModules();
#endif
}

bool swift::performTypeLocChecking(ASTContext &Ctx, TypeLoc &T,
                                   bool isSILType, DeclContext *DC,
                                   bool ProduceDiagnostics) {
  TypeResolutionOptions options;
  if (isSILType)
    options |= TR_SILType;

  if (ProduceDiagnostics) {
    return TypeChecker(Ctx).validateType(T, DC, options);
  } else {
    // Set up a diagnostics engine that swallows diagnostics.
    DiagnosticEngine Diags(Ctx.SourceMgr);
    return TypeChecker(Ctx, Diags).validateType(T, DC, options);
  }
}

bool swift::typeCheckCompletionDecl(Decl *D) {
  auto &Ctx = D->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);
  TypeChecker TC(Ctx, Diags);

  TC.typeCheckDecl(D, true);
  return true;
}

bool swift::typeCheckCompletionContextExpr(ASTContext &Ctx, DeclContext *DC,
                                           Expr *&parsedExpr) {
  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, diags);
  TC.typeCheckExpression(parsedExpr, DC, Type(), /*discardedExpr=*/true,
                         FreeTypeVariableBinding::GenericParameters);
  
  return parsedExpr && !isa<ErrorExpr>(parsedExpr)
                    && parsedExpr->getType()
                    && !parsedExpr->getType()->is<ErrorType>();
}

bool swift::typeCheckAbstractFunctionBodyUntil(AbstractFunctionDecl *AFD,
                                               SourceLoc EndTypeCheckLoc) {
  auto &Ctx = AFD->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, Diags);
  return !TC.typeCheckAbstractFunctionBodyUntil(AFD, EndTypeCheckLoc);
}

bool swift::typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
  auto &Ctx = static_cast<Decl *>(TLCD)->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, Diags);
  TC.typeCheckTopLevelCodeDecl(TLCD);
  return true;
}

static void deleteTypeCheckerAndDiags(LazyResolver *resolver) {
  DiagnosticEngine &diags = static_cast<TypeChecker*>(resolver)->Diags;
  delete resolver;
  delete &diags;
}

OwnedResolver swift::createLazyResolver(ASTContext &Ctx) {
  auto diags = new DiagnosticEngine(Ctx.SourceMgr);
  return OwnedResolver(new TypeChecker(Ctx, *diags),
                       &deleteTypeCheckerAndDiags);
}

void TypeChecker::diagnoseAmbiguousMemberType(Type baseTy,
                                              SourceRange baseRange,
                                              Identifier name,
                                              SourceLoc nameLoc,
                                              LookupTypeResult &lookup) {
  diagnose(nameLoc, diag::ambiguous_member_type, name, baseTy)
    .highlight(baseRange);
  for (const auto &member : lookup) {
    diagnose(member.first, diag::found_candidate_type,
             member.second);
  }
}
