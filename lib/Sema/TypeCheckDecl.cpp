//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
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
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

/// \brief Describes the kind of implicit constructor that will be
/// generated.
enum class ImplicitConstructorKind {
  /// \brief The default constructor, which default-initializes each
  /// of the instance variables.
  Default,
  /// \brief The memberwise constructor, which initializes each of
  /// the instance variables from a parameter of the same type and
  /// name.
  Memberwise
};

/// \brief Determine whether the given pattern contains only a single variable
/// that is a property.
static bool isPatternProperty(Pattern *pattern) {
  pattern = pattern->getSemanticsProvidingPattern();
  if (auto named = dyn_cast<NamedPattern>(pattern))
    return named->getDecl()->isProperty();

  return false;
}

/// Determine whether the given declaration can inherit a class.
static bool canInheritClass(Decl *decl) {
  if (isa<ClassDecl>(decl))
    return true;

  // FIXME: Can any typealias declare inheritance from a class?
  if (auto typeAlias = dyn_cast<TypeAliasDecl>(decl))
    return typeAlias->isGenericParameter();

  return false;
}

/// Retrieve the declared type of a type declaration or extension.
static Type getDeclaredType(Decl *decl) {
  if (auto typeDecl = dyn_cast<TypeDecl>(decl))
    return typeDecl->getDeclaredType();
  return cast<ExtensionDecl>(decl)->getExtendedType();
}

/// Determine whether the given declaration already had its inheritance
/// clause checked.
static bool alreadyCheckedInheritanceClause(Decl *decl) {
  // FIXME: Should we simply record when the inheritance clause was checked,
  // so we don't need this approximation?

  // If it's an extension with a non-empty protocol list, we're done.
  if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
    return !ext->getProtocols().empty();
  }

  // If it's a nominal declaration with a non-empty protocol list, we're done.
  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    if (!nominal->getProtocols().empty())
      return true;

    // If it's a class declaration with a superclass, we're done.
    if (auto classDecl = dyn_cast<ClassDecl>(nominal))
      return classDecl->hasSuperclass();

    return false;
  }

  // For a typealias with a non-empty protocol list, we're done.
  auto typeAlias = cast<TypeAliasDecl>(decl);
  if (!typeAlias->getProtocols().empty())
    return true;

  // For a generic parameter with a superclass, we're done.
  return typeAlias->isGenericParameter() && typeAlias->getSuperclass();
}

/// Retrieve the inheritance clause as written from the given declaration.
static MutableArrayRef<TypeLoc> getInheritanceClause(Decl *decl) {
  if (auto ext = dyn_cast<ExtensionDecl>(decl))
    return ext->getInherited();

  return cast<TypeDecl>(decl)->getInherited();
}

/// Check the inheritance clause of a type declaration or extension thereof.
///
/// This routine validates all of the types in the parsed inheritance clause,
/// recording the superclass (if any and if allowed) as well as the protocols
/// to which this type declaration conforms.
static void checkInheritanceClause(TypeChecker &tc, Decl *decl) {
  // If we already checked the inheritance clause, we're done.
  if (alreadyCheckedInheritanceClause(decl))
    return;

  // Check all of the types listed in the inheritance clause.
  Type superclassTy;
  SourceRange superclassRange;
  llvm::SmallSetVector<ProtocolDecl *, 4> allProtocols;
  llvm::SmallDenseMap<CanType, SourceRange> inheritedTypes;
  auto inheritedClause = getInheritanceClause(decl);
  for (unsigned i = 0, n = inheritedClause.size(); i != n; ++i) {
    auto &inherited = inheritedClause[i];

    // Validate the type.
    if (tc.validateType(inherited)) {
      inherited.setInvalidType(tc.Context);
      continue;
    }

    auto inheritedTy = inherited.getType();

    // Check whether we inherited from the same type twice.
    CanType inheritedCanTy = inheritedTy->getCanonicalType();
    auto knownType = inheritedTypes.find(inheritedCanTy);
    if (knownType != inheritedTypes.end()) {
      SourceLoc afterPriorLoc
        = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                     inheritedClause[i-1].getSourceRange().End);
      SourceLoc afterMyEndLoc
        = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                     inherited.getSourceRange().End);

      tc.diagnose(inherited.getSourceRange().Start,
                  diag::duplicate_inheritance, inheritedTy)
        .fixItRemove(Diagnostic::Range(afterPriorLoc, afterMyEndLoc))
        .highlight(knownType->second);
      continue;
    }
    inheritedTypes[inheritedCanTy] = inherited.getSourceRange();

    // If this is a protocol or protocol composition type, record the
    // protocols.
    if (inheritedTy->isExistentialType()) {
      SmallVector<ProtocolDecl *, 4> protocols;
      inheritedTy->isExistentialType(protocols);
      allProtocols.insert(protocols.begin(), protocols.end());
      continue;
    }

    // If this is a class type, it may be the superclass.
    if (inheritedTy->getClassOrBoundGenericClass()) {
      // First, check if we already had a superclass.
      if (superclassTy) {
        // FIXME: Check for shadowed protocol names, i.e., NSObject?

        // Complain about multiple inheritance.
        // Don't emit a Fix-It here. The user has to think harder about this.
        tc.diagnose(inherited.getSourceRange().Start,
                    diag::multiple_inheritance, superclassTy, inheritedTy)
          .highlight(superclassRange);
        continue;
      }

      // If the declaration we're looking at doesn't allow a superclass,
      // complain.
      //
      // FIXME: Allow type aliases to 'inherit' from classes, as an additional
      // kind of requirement?
      if (!canInheritClass(decl)) {
        tc.diagnose(decl->getLoc(),
                    isa<ExtensionDecl>(decl)
                      ? diag::extension_class_inheritance
                      : diag::non_class_inheritance,
                    getDeclaredType(decl), inheritedTy)
          .highlight(inherited.getSourceRange());
        continue;
      }

      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        SourceLoc afterPriorLoc
          = Lexer::getLocForEndOfToken(
              tc.Context.SourceMgr,
              inheritedClause[i-1].getSourceRange().End);
        SourceLoc afterMyEndLoc
          = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                       inherited.getSourceRange().End);

        tc.diagnose(inherited.getSourceRange().Start,
                    diag::superclass_not_first, inheritedTy)
          .fixItRemove(Diagnostic::Range(afterPriorLoc, afterMyEndLoc))
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the superclass.
      }

      // Record the superclass.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      continue;
    }

    // If this is an error type, ignore it.
    if (inheritedTy->is<ErrorType>())
      continue;

    // We can't inherit from a non-class, non-protocol type.
    tc.diagnose(decl->getLoc(),
                canInheritClass(decl)
                  ? diag::inheritance_from_non_protocol_or_class
                  : diag::inheritance_from_non_protocol,
                inheritedTy);
    // FIXME: Note pointing to the declaration 'inheritedTy' references?
  }

  // Record the protocols to which this declaration conforms along with the
  // superclass.
  if (allProtocols.empty() && !superclassTy)
    return;

  auto allProtocolsCopy = tc.Context.AllocateCopy(allProtocols);
  if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
    assert(!superclassTy && "Extensions can't add superclasses");
    ext->setProtocols(allProtocolsCopy);
    return;
  }

  auto typeDecl = cast<TypeDecl>(decl);
  typeDecl->setProtocols(allProtocolsCopy);
  if (superclassTy) {
    if (auto classDecl = dyn_cast<ClassDecl>(decl))
      classDecl->setSuperclass(superclassTy);
    else
      cast<TypeAliasDecl>(decl)->setSuperclass(superclassTy);
  }

  // For protocols, generic parameters, and associated types, fill in null
  // conformances.
  if (isa<ProtocolDecl>(decl) ||
      (isa<TypeAliasDecl>(decl) &&
       (isa<ProtocolDecl>(decl->getDeclContext()) ||
        cast<TypeAliasDecl>(decl)->isGenericParameter()))) {
     // Set null conformances.
     unsigned conformancesSize
         = sizeof(ProtocolConformance *) * allProtocols.size();
     ProtocolConformance **conformances
         = (ProtocolConformance **)tc.Context.Allocate(
             conformancesSize,
             alignof(ProtocolConformance *));
     memset(conformances, 0, conformancesSize);
     cast<TypeDecl>(decl)->setConformances(
       llvm::makeArrayRef(conformances, allProtocols.size()));
  }
}

namespace {

class DeclChecker : public DeclVisitor<DeclChecker> {
  
public:
  TypeChecker &TC;

  // For library-style parsing, we need to make two passes over the global
  // scope.  These booleans indicate whether this is currently the first or
  // second pass over the global scope (or neither, if we're in a context where
  // we only visit each decl once).
  bool IsFirstPass;
  bool IsSecondPass;

  DeclChecker(TypeChecker &TC, bool IsFirstPass, bool IsSecondPass)
      : TC(TC), IsFirstPass(IsFirstPass), IsSecondPass(IsSecondPass) {}

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//

  void validateAttributes(ValueDecl *VD);

  template<typename DeclType>
  void gatherExplicitConformances(DeclType *D, Type T) {
    llvm::SmallPtrSet<ProtocolDecl *, 4> knownProtocols;
    SmallVector<ProtocolDecl *, 4> allProtocols;
    SmallVector<ProtocolConformance *, 4> allConformances;
    for (auto inherited : D->getInherited()) {
      SmallVector<ProtocolDecl *, 4> protocols;
      if (inherited.getType()->isExistentialType(protocols)) {
        for (auto proto : protocols) {
          if (knownProtocols.insert(proto)) {
            ProtocolConformance *conformance = nullptr;
            if (TC.conformsToProtocol(T, proto, &conformance,
                                      D->getStartLoc(), D)) {
              // For nominal types and extensions thereof, record conformance.
              if (isa<NominalTypeDecl>(D) || isa<ExtensionDecl>(D))
                TC.Context.recordConformance(proto, D);
            }
            allProtocols.push_back(proto);
            allConformances.push_back(conformance);
          }
        }
      }
    }

    // Set the protocols and conformances.
    if (D->getProtocols().size() == allProtocols.size()) {
      // Do nothing: we've already set the list of protocols.
      assert(std::equal(D->getProtocols().begin(), D->getProtocols().end(),
                        allProtocols.begin()) && "Protocol list changed?");
    } else {
      D->setProtocols(D->getASTContext().AllocateCopy(allProtocols));
    }
    D->setConformances(D->getASTContext().AllocateCopy(allConformances));
  }
  
  void checkExplicitConformance(TypeDecl *D, Type T) {
    gatherExplicitConformances(D, T);
  }
  
  void checkExplicitConformance(ExtensionDecl *D, Type T) {
    gatherExplicitConformances(D, T);
  }

  void checkGenericParams(GenericParamList *GenericParams) {
    assert(GenericParams && "Missing generic parameters");

    // Assign archetypes to each of the generic parameters.
    ArchetypeBuilder Builder(TC.Context, TC.Diags);
    unsigned Index = 0;
    for (auto GP : *GenericParams) {
      auto TypeParam = GP.getAsTypeParam();

      // Check the constraints on the type parameter.
      checkInheritanceClause(TC, TypeParam);

      // Add the generic parameter to the builder.
      Builder.addGenericParameter(TypeParam, Index++);
    }

    // Add the requirements clause to the builder, validating only those
    // types that need to be complete at this point.
    // FIXME: Tell the type validator not to assert about unresolved types.
    for (auto &Req : GenericParams->getRequirements()) {
      if (Req.isInvalid())
        continue;
      
      switch (Req.getKind()) {
      case RequirementKind::Conformance: {
        if (TC.validateType(Req.getConstraintLoc())) {
          Req.setInvalid();
          continue;
        }

        if (!Req.getConstraint()->isExistentialType() &&
            !Req.getConstraint()->getClassOrBoundGenericClass()) {
          TC.diagnose(GenericParams->getWhereLoc(),
                      diag::requires_conformance_nonprotocol,
                      Req.getSubjectLoc(), Req.getConstraintLoc());
          Req.getConstraintLoc().setInvalidType(TC.Context);
          Req.setInvalid();
          continue;
        }
        break;
      }

      case RequirementKind::SameType:
        break;
      }

      if (Builder.addRequirement(Req))
        Req.setInvalid();
    }

    // Wire up the archetypes.
    Builder.assignArchetypes();
    for (auto GP : *GenericParams) {
      auto TypeParam = GP.getAsTypeParam();

      TypeParam->getUnderlyingTypeLoc()
        = TypeLoc::withoutLoc(Builder.getArchetype(TypeParam));
    }
    GenericParams->setAllArchetypes(
      TC.Context.AllocateCopy(Builder.getAllArchetypes()));

    // Validate the types in the requirements clause.
    for (auto &Req : GenericParams->getRequirements()) {
      if (Req.isInvalid())
        continue;

      switch (Req.getKind()) {
      case RequirementKind::Conformance: {
        if (TC.validateType(Req.getSubjectLoc())) {
          Req.setInvalid();
          continue;
        }
        break;
      }

      case RequirementKind::SameType:
        if (TC.validateType(Req.getFirstTypeLoc())) {
          Req.setInvalid();
          continue;
        }

        if (TC.validateType(Req.getSecondTypeLoc())) {
          Req.setInvalid();
          continue;
        }
        break;
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  void visitImportDecl(ImportDecl *ID) {
    // Nothing to do.
  }

  void visitBoundVars(Pattern *P) {
    switch (P->getKind()) {
    // Recur into patterns.
    case PatternKind::Tuple:
      for (auto &field : cast<TuplePattern>(P)->getFields())
        visitBoundVars(field.getPattern());
      return;
    case PatternKind::Paren:
      return visitBoundVars(cast<ParenPattern>(P)->getSubPattern());
    case PatternKind::Typed:
      return visitBoundVars(cast<TypedPattern>(P)->getSubPattern());
    case PatternKind::NominalType:
      return visitBoundVars(cast<NominalTypePattern>(P)->getSubPattern());
    case PatternKind::UnionElement: {
      auto *OP = cast<UnionElementPattern>(P);
      if (OP->hasSubPattern())
        visitBoundVars(OP->getSubPattern());
      return;
    }
    case PatternKind::Var:
      return visitBoundVars(cast<VarPattern>(P)->getSubPattern());

    // Handle vars.
    case PatternKind::Named: {
      VarDecl *VD = cast<NamedPattern>(P)->getDecl();

      if (!VD->getType()->isMaterializable()) {
        TC.diagnose(VD->getStartLoc(), diag::var_type_not_materializable,
                    VD->getType());
        VD->overwriteType(ErrorType::get(TC.Context));
      }

      validateAttributes(VD);
      
      // The var requires ObjC interop if it has an [objc] or [iboutlet]
      // attribute or if it's a member of an ObjC class.
      DeclContext *dc = VD->getDeclContext();
      if (dc && dc->getDeclaredTypeInContext()) {
        ClassDecl *classContext = dc->getDeclaredTypeInContext()
          ->getClassOrBoundGenericClass();
        VD->setIsObjC(VD->getAttrs().isObjC()
                      || (classContext && classContext->isObjC()));
      }
      
      return;
    }

    // Handle non-vars.
    case PatternKind::Any:
    case PatternKind::Isa:
    case PatternKind::Expr:
      return;
    }
    llvm_unreachable("bad pattern kind!");
  }

  void setBoundVarsTypeError(Pattern *pattern) {
    switch (pattern->getKind()) {
    case PatternKind::Tuple:
      for (auto &field : cast<TuplePattern>(pattern)->getFields())
        setBoundVarsTypeError(field.getPattern());
      return;
    case PatternKind::Paren:
      return setBoundVarsTypeError(cast<ParenPattern>(pattern)->getSubPattern());
    case PatternKind::Typed:
      return setBoundVarsTypeError(cast<TypedPattern>(pattern)->getSubPattern());
    case PatternKind::NominalType:
      return setBoundVarsTypeError(cast<NominalTypePattern>(pattern)
                                     ->getSubPattern());
    case PatternKind::Var:
      return setBoundVarsTypeError(cast<VarPattern>(pattern)->getSubPattern());
    case PatternKind::UnionElement:
      if (auto subpattern = cast<UnionElementPattern>(pattern)->getSubPattern())
        setBoundVarsTypeError(subpattern);
      return;
        
    // Handle vars.
    case PatternKind::Named: {
      VarDecl *var = cast<NamedPattern>(pattern)->getDecl();
      if (!var->hasType())
        var->setType(ErrorType::get(TC.Context));
      return;
    }

    // Handle non-vars.
    case PatternKind::Any:
    case PatternKind::Isa:
    case PatternKind::Expr:
      return;
    }
    llvm_unreachable("bad pattern kind!");
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool DelayCheckingPattern =
      TC.TU.Kind != TranslationUnit::Library &&
      TC.TU.Kind != TranslationUnit::SIL &&
      PBD->getDeclContext()->isModuleContext();
    if (IsSecondPass && !DelayCheckingPattern) {
      if (PBD->getInit() && PBD->getPattern()->hasType()) {
        Expr *Init = PBD->getInit();
        Type DestTy = PBD->getPattern()->getType();
        if (TC.typeCheckExpression(Init, PBD->getDeclContext(), DestTy)) {
          if (DestTy)
            TC.diagnose(PBD, diag::while_converting_var_init,
                        DestTy);
        } else {
          PBD->setInit(Init);
        }
      }
      return;
    }

    // If there is no initializer and we are not in a type context,
    // create a default initializer.
    if (!PBD->getInit() && !IsFirstPass &&
        isa<TypedPattern>(PBD->getPattern()) &&
        !PBD->getDeclContext()->isTypeContext()) {
      // Type-check the pattern.
      if (TC.typeCheckPattern(PBD->getPattern(),
                              PBD->getDeclContext(),
                              /*allowUnknownTypes*/false))
        return;

      Type ty = PBD->getPattern()->getType();
      Expr *initializer = nullptr;
      if (isPatternProperty(PBD->getPattern())) {
        // Properties don't have initializers.
      } else if (!TC.isDefaultInitializable(ty, &initializer)) {
        // FIXME: Better diagnostics here.
        TC.diagnose(PBD, diag::decl_no_default_init, ty);
        PBD->setInvalid();
      } else {
        if (TC.typeCheckExpression(initializer, PBD->getDeclContext(), ty)) {
          TC.diagnose(PBD, diag::while_converting_var_init, ty);
          return;
        }

        PBD->setInit(initializer);
      }
    } else if (PBD->getInit() && !IsFirstPass) {
      Type DestTy;
      if (isa<TypedPattern>(PBD->getPattern())) {
        if (TC.typeCheckPattern(PBD->getPattern(),
                                PBD->getDeclContext(),
                                /*allowUnknownTypes*/false))
          return;
        DestTy = PBD->getPattern()->getType();
      }
      Expr *Init = PBD->getInit();
      if (TC.typeCheckExpression(Init, PBD->getDeclContext(), DestTy)) {
        if (DestTy)
          TC.diagnose(PBD, diag::while_converting_var_init,
                      DestTy);
        setBoundVarsTypeError(PBD->getPattern());
        return;
      }
      if (!DestTy) {
        Expr *newInit = TC.coerceToMaterializable(Init);
        if (newInit) Init = newInit;
      }
      PBD->setInit(Init);
      if (!DestTy) {
        if (TC.coerceToType(PBD->getPattern(),
                            PBD->getDeclContext(),
                            Init->getType()))
          return;
      }
    } else if (!IsFirstPass || !DelayCheckingPattern) {
      if (TC.typeCheckPattern(PBD->getPattern(),
                              PBD->getDeclContext(),
                              /*allowUnknownTypes*/false))
        return;
    }

    visitBoundVars(PBD->getPattern());

  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    if (IsSecondPass)
      return;

    assert(SD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent subscripts outside of types!");

    bool isInvalid = TC.validateType(SD->getElementTypeLoc());
    isInvalid |= TC.typeCheckPattern(SD->getIndices(),
                                     SD->getDeclContext(),
                                     /*allowUnknownTypes*/false);

    if (isInvalid) {
      SD->setType(ErrorType::get(TC.Context));
    } else {
      SD->setType(FunctionType::get(SD->getIndices()->getType(),
                                    SD->getElementType(), TC.Context));
    }
  }
  
  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    if (!IsSecondPass) {
      TC.validateType(TAD->getUnderlyingTypeLoc());
      if (!isa<ProtocolDecl>(TAD->getDeclContext()))
        checkInheritanceClause(TC, TAD);
    }

    if (!IsFirstPass)
      checkExplicitConformance(TAD, TAD->getDeclaredType());
  }

  void visitUnionDecl(UnionDecl *OOD) {
    if (!IsSecondPass) {
      if (auto gp = OOD->getGenericParams()) {
        gp->setOuterParameters(
                            OOD->getDeclContext()->getGenericParamsOfContext());
        checkGenericParams(gp);
      }

      // Now that we have archetypes for our generic parameters (including
      // generic parameters from outer scopes), we can canonicalize our type.
      OOD->overwriteType(OOD->getType()->getCanonicalType());
      OOD->overwriteDeclaredType(OOD->getDeclaredType()->getCanonicalType());
      TC.validateTypeSimple(OOD->getDeclaredTypeInContext());

      validateAttributes(OOD);

      checkInheritanceClause(TC, OOD);
    }
    
    for (Decl *member : OOD->getMembers())
      visit(member);
    
    if (!IsFirstPass)
      checkExplicitConformance(OOD, OOD->getDeclaredTypeInContext());
  }

  void visitStructDecl(StructDecl *SD) {
    if (!IsSecondPass) {
      if (auto gp = SD->getGenericParams()) {
        gp->setOuterParameters(
                             SD->getDeclContext()->getGenericParamsOfContext());
        checkGenericParams(gp);
      }

      // Now that we have archetypes for our generic parameters (including
      // generic parameters from outer scopes), we can canonicalize our type.
      SD->overwriteType(SD->getType()->getCanonicalType());
      SD->overwriteDeclaredType(SD->getDeclaredType()->getCanonicalType());
      TC.validateTypeSimple(SD->getDeclaredTypeInContext());

      validateAttributes(SD);

      checkInheritanceClause(TC, SD);
    }

    // Visit each of the members.
    for (Decl *Member : SD->getMembers()) {
      visit(Member);
    }

    if (!IsSecondPass) {
      TC.addImplicitConstructors(SD);
    }

    if (!IsFirstPass)
      checkExplicitConformance(SD, SD->getDeclaredTypeInContext());
  }
  
  void checkObjCConformance(ProtocolDecl *protocol,
                            ProtocolConformance *conformance) {
    if (!conformance)
      return;
    if (protocol->isObjC())
      for (auto &mapping : conformance->getWitnesses())
        mapping.second.Decl->setIsObjC(true);
    for (auto &inherited : conformance->getInheritedConformances())
      checkObjCConformance(inherited.first, inherited.second);
  }
  
  /// Mark class members needed to conform to ObjC protocols as requiring ObjC
  /// interop.
  void checkObjCConformances(ArrayRef<ProtocolDecl*> protocols,
                             ArrayRef<ProtocolConformance*> conformances) {
    assert(protocols.size() == conformances.size() &&
           "protocol conformance mismatch");
    
    for (unsigned i = 0, size = protocols.size(); i < size; ++i)
      checkObjCConformance(protocols[i], conformances[i]);
  }

  void visitClassDecl(ClassDecl *CD) {
    if (!IsSecondPass) {
      // Check our generic parameters first.
      if (auto gp = CD->getGenericParams()) {
        gp->setOuterParameters(
          CD->getDeclContext()->getGenericParamsOfContext());
        checkGenericParams(gp);
      }

      // Now that we have archetypes for our generic parameters (including
      // generic parameters from outer scopes), we can canonicalize our type.
      CD->overwriteType(CD->getType()->getCanonicalType());
      CD->overwriteDeclaredType(CD->getDeclaredType()->getCanonicalType());
      TC.validateTypeSimple(CD->getDeclaredTypeInContext());

      validateAttributes(CD);
      checkInheritanceClause(TC, CD);

      ClassDecl *superclassDecl = CD->hasSuperclass()
        ? CD->getSuperclass()->getClassOrBoundGenericClass()
        : nullptr;
      
      CD->setIsObjC(CD->getAttrs().isObjC()
                    || (superclassDecl && superclassDecl->isObjC()));
    }

    for (Decl *Member : CD->getMembers())
      visit(Member);
    
    if (!IsFirstPass) {
      checkExplicitConformance(CD, CD->getDeclaredTypeInContext());
      checkObjCConformances(CD->getProtocols(), CD->getConformances());
    }
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    if (IsSecondPass) {
      return;
    }
    
    // If the protocol is [objc], it may only refine other [objc] protocols.
    // FIXME: Revisit this restriction.
    if (PD->getAttrs().isObjC()) {
      bool isObjC = true;

      SmallVector<ProtocolDecl*, 2> inheritedProtocols;
      for (auto inherited : PD->getInherited()) {
        if (!inherited.getType()->isExistentialType(inheritedProtocols))
          continue;
        
        for (auto proto : inheritedProtocols) {
          if (!proto->getAttrs().isObjC()) {
            TC.diagnose(PD->getLoc(),
                        diag::objc_protocol_inherits_non_objc_protocol,
                        PD->getDeclaredType(), proto->getDeclaredType());
            TC.diagnose(proto->getLoc(),
                        diag::protocol_here,
                        proto->getName());
            isObjC = false;
          }
        }
        
        inheritedProtocols.clear();
      }
      
      PD->setIsObjC(isObjC);
    }

    // Fix the 'This' associated type.
    TypeAliasDecl *thisDecl = nullptr;
    for (auto Member : PD->getMembers()) {
      if (auto AssocType = dyn_cast<TypeAliasDecl>(Member)) {
        if (AssocType->getName().str() == "This") {
          thisDecl = AssocType;
          break;
        }
      }
    }

    // Build archetypes for this protocol.
    ArchetypeBuilder builder(TC.Context, TC.Diags);
    builder.addGenericParameter(thisDecl, 0);
    builder.addImplicitConformance(thisDecl, PD);
    builder.assignArchetypes();

    // Set the underlying type of each of the associated types to the
    // appropriate archetype.
    ArchetypeType *thisArchetype = builder.getArchetype(thisDecl);
    for (auto member : PD->getMembers()) {
      if (auto assocType = dyn_cast<TypeAliasDecl>(member)) {
        TypeLoc underlyingTy;
        if (assocType == thisDecl)
          underlyingTy = TypeLoc::withoutLoc(thisArchetype);
        else
          underlyingTy = TypeLoc::withoutLoc(thisArchetype->getNestedType(
                                               assocType->getName()));
        assocType->getUnderlyingTypeLoc() = underlyingTy;
      }
    }

    // Check the members.
    for (auto Member : PD->getMembers())
      visit(Member);

    validateAttributes(PD);
  }
  
  void visitVarDecl(VarDecl *VD) {
    // Delay type-checking on VarDecls until we see the corresponding
    // PatternBindingDecl.
  }

  bool semaFuncParamPatterns(DeclContext *dc,
                             ArrayRef<Pattern*> paramPatterns) {
    bool badType = false;
    for (Pattern *P : paramPatterns) {
      if (P->hasType())
        continue;
      if (TC.typeCheckPattern(P, dc, false)) {
        badType = true;
        continue;
      }
    }
    return badType;
  }

  /// \brief Validate and consume the attributes that are applicable to the
  /// AnyFunctionType.
  ///
  /// Currently, we only allow 'noreturn' to be applied on a FuncDecl.
  AnyFunctionType::ExtInfo
  validateAndConsumeFunctionTypeAttributes(FuncDecl *FD) {
    DeclAttributes &Attrs = FD->getMutableAttrs();
    auto Info = AnyFunctionType::ExtInfo();

    if (Attrs.hasCC()) {
      TC.diagnose(FD->getStartLoc(), diag::invalid_decl_attribute, "cc");
      Attrs.cc = {};
    }

    if (Attrs.isThin()) {
      TC.diagnose(FD->getStartLoc(), diag::invalid_decl_attribute,
                  "thin");
      Attrs.Thin = false;
    }

    // 'noreturn' is allowed on a function declaration.
    Info = Info.withIsNoReturn(Attrs.isNoReturn());
    Attrs.NoReturn = false;

    if (Attrs.isAutoClosure()) {
      TC.diagnose(FD->getStartLoc(), diag::invalid_decl_attribute,
                  "auto_closure");
      Attrs.AutoClosure = false;
    }

    if (Attrs.isObjCBlock()) {
      TC.diagnose(FD->getStartLoc(), diag::invalid_decl_attribute,
                  "objc_block");
      Attrs.ObjCBlock = false;
    }

    return Info;
  }

  void semaFuncExpr(FuncDecl *FD) {
    FuncExpr *FE = FD->getBody();

    if (FE->getType())
      return;

    bool badType = false;
    if (!FE->getBodyResultTypeLoc().isNull()) {
      if (TC.validateType(FE->getBodyResultTypeLoc())) {
        badType = true;
      }
    }

    badType = badType || semaFuncParamPatterns(FE, FE->getArgParamPatterns());
    badType = badType || semaFuncParamPatterns(FE, FE->getBodyParamPatterns());

    if (badType) {
      FE->setType(ErrorType::get(TC.Context));
      return;
    }

    Type funcTy = FE->getBodyResultTypeLoc().getType();
    if (!funcTy) {
      funcTy = TupleType::getEmpty(TC.Context);
    }

    // FIXME: it would be nice to have comments explaining what this is all about.
    auto patterns = FE->getArgParamPatterns();
    bool isInstanceFunc = false;
    GenericParamList *genericParams = nullptr;
    GenericParamList *outerGenericParams = nullptr;
    if (FuncDecl *FD = FE->getDecl()) {
      isInstanceFunc = (bool)FD->computeThisType(&outerGenericParams);
      genericParams = FD->getGenericParams();
    }

    for (unsigned i = 0, e = patterns.size(); i != e; ++i) {
      Type argTy = patterns[e - i - 1]->getType();

      GenericParamList *params = nullptr;
      if (e - i - 1 == isInstanceFunc && genericParams) {
        params = genericParams;
      } else if (e - i - 1 == 0 && outerGenericParams) {
        params = outerGenericParams;
      }

      // Validate and consume the function type attributes.
      auto Info = validateAndConsumeFunctionTypeAttributes(FD);
      if (params) {
        funcTy = PolymorphicFunctionType::get(argTy, funcTy,
                                              params,
                                              Info,
                                              TC.Context);
      } else {
        funcTy = FunctionType::get(argTy, funcTy, Info, TC.Context);
      }

    }
    FE->setType(funcTy);
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (IsSecondPass)
      return;

    FuncExpr *body = FD->getBody();

    // Before anything else, set up the 'this' argument correctly.
    GenericParamList *outerGenericParams = nullptr;
    if (Type thisType = FD->computeThisType(&outerGenericParams)) {
      TypedPattern *thisPattern =
        cast<TypedPattern>(body->getArgParamPatterns()[0]);
      if (thisPattern->hasType()) {
        assert(thisPattern->getType().getPointer() == thisType.getPointer());
      } else {
        thisPattern->setType(thisType);
        cast<NamedPattern>(thisPattern->getSubPattern())->setType(thisType);
      }
    }

    if (auto gp = FD->getGenericParams()) {
      gp->setOuterParameters(outerGenericParams);
      checkGenericParams(gp);
    }

    semaFuncExpr(FD);
    FD->setType(body->getType());

    validateAttributes(FD);
    
    // A method is ObjC-compatible if it's explicitly [objc], a member of an
    // ObjC-compatible class, or an accessor for an ObjC property.
    DeclContext *dc = FD->getDeclContext();
    if (dc && dc->getDeclaredTypeInContext()) {
      ClassDecl *classContext = dc->getDeclaredTypeInContext()
        ->getClassOrBoundGenericClass();
      
      bool isObjC = FD->getAttrs().isObjC()
        || (classContext && classContext->isObjC());
      if (!isObjC && FD->isGetterOrSetter()) {
        // If the property decl is an instance property, its accessors will
        // be instance methods and the above condition will mark them ObjC.
        // The only additional condition we need to check is if the var decl
        // had an [objc] or [iboutlet] property. We don't use prop->isObjC()
        // because the property accessors may be visited before the VarDecl and
        // prop->isObjC() may not yet be set by typechecking.
        ValueDecl *prop = cast<ValueDecl>(FD->getGetterOrSetterDecl());
        isObjC = prop->getAttrs().isObjC() || prop->getAttrs().isIBOutlet();
      }
      
      FD->setIsObjC(isObjC);
    }
  }

  void visitUnionElementDecl(UnionElementDecl *ED) {
    if (IsSecondPass)
      return;

    UnionDecl *OOD = cast<UnionDecl>(ED->getDeclContext());
    Type ElemTy = OOD->getDeclaredTypeInContext();

    if (!ED->getArgumentTypeLoc().isNull())
      if (TC.validateType(ED->getArgumentTypeLoc()))
        return;
    if (!ED->getResultTypeLoc().isNull())
      if (TC.validateType(ED->getResultTypeLoc()))
        return;

    // If we have a simple element, just set the type.
    if (ED->getArgumentType().isNull()) {
      Type argTy = MetaTypeType::get(ElemTy, TC.Context);
      Type fnTy;
      if (auto gp = OOD->getGenericParamsOfContext())
        fnTy = PolymorphicFunctionType::get(argTy, ElemTy, gp, TC.Context);
      else
        fnTy = FunctionType::get(argTy, ElemTy, TC.Context);
      ED->setType(fnTy);
      return;
    }

    Type fnTy = FunctionType::get(ED->getArgumentType(), ElemTy, TC.Context);
    if (auto gp = OOD->getGenericParamsOfContext())
      fnTy = PolymorphicFunctionType::get(MetaTypeType::get(ElemTy, TC.Context),
                                          fnTy, gp, TC.Context);
    else
      fnTy = FunctionType::get(MetaTypeType::get(ElemTy, TC.Context), fnTy,
                               TC.Context);
    ED->setType(fnTy);

    // Require the carried type to be materializable.
    if (!ED->getArgumentType()->isMaterializable()) {
      TC.diagnose(ED->getLoc(), diag::union_element_not_materializable);
    }
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    if (ED->isInvalid()) {
      // Mark children as invalid.
      // FIXME: This is awful.
      for (auto member : ED->getMembers())
        member->setInvalid();
      return;
    }

    if (!IsSecondPass) {
      CanType ExtendedTy = ED->getExtendedType()->getCanonicalType();

      // FIXME: we should require generic parameter clauses here
      if (auto unbound = dyn_cast<UnboundGenericType>(ExtendedTy)) {
        auto boundType = unbound->getDecl()->getDeclaredTypeInContext();
        ED->getExtendedTypeLoc() = TypeLoc::withoutLoc(boundType);
        ExtendedTy = boundType->getCanonicalType();
      }

      if (!isa<UnionType>(ExtendedTy) &&
          !isa<StructType>(ExtendedTy) &&
          !isa<ClassType>(ExtendedTy) &&
          !isa<BoundGenericUnionType>(ExtendedTy) &&
          !isa<BoundGenericStructType>(ExtendedTy) &&
          !isa<BoundGenericClassType>(ExtendedTy) &&
          !isa<ErrorType>(ExtendedTy)) {
        TC.diagnose(ED->getStartLoc(), diag::non_nominal_extension,
                    isa<ProtocolType>(ExtendedTy), ExtendedTy);
        // FIXME: It would be nice to point out where we found the named type
        // declaration, if any.
      }

      // Add this extension to the list of extensions for the extended type.
      if (auto nominal = ExtendedTy->getAnyNominal()) {
        nominal->addExtension(ED);
      }
    
      checkInheritanceClause(TC, ED);
    }

    for (Decl *Member : ED->getMembers())
      visit(Member);

    if (!IsFirstPass) {
      checkExplicitConformance(ED, ED->getExtendedType());
      checkObjCConformances(ED->getProtocols(), ED->getConformances());
    }
  }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
    // See swift::performTypeChecking for TopLevelCodeDecl handling.
    llvm_unreachable("TopLevelCodeDecls are handled elsewhere");
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    if (IsSecondPass)
      return;

    assert(CD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent constructors outside of types!");

    GenericParamList *outerGenericParams = nullptr;
    Type ThisTy = CD->computeThisType(&outerGenericParams);
    TC.validateTypeSimple(ThisTy);
    CD->getImplicitThisDecl()->setType(ThisTy);

    if (auto gp = CD->getGenericParams()) {
      gp->setOuterParameters(outerGenericParams);
      checkGenericParams(gp);
    }

    if (TC.typeCheckPattern(CD->getArguments(),
                            CD,
                            /*allowUnknownTypes*/false)) {
      CD->setType(ErrorType::get(TC.Context));
    } else {
      Type FnTy;
      Type AllocFnTy;
      Type InitFnTy;
      if (GenericParamList *innerGenericParams = CD->getGenericParams()) {
        innerGenericParams->setOuterParameters(outerGenericParams);
        FnTy = PolymorphicFunctionType::get(CD->getArguments()->getType(),
                                            ThisTy, innerGenericParams,
                                            TC.Context);
      } else
        FnTy = FunctionType::get(CD->getArguments()->getType(),
                                 ThisTy, TC.Context);
      Type ThisMetaTy = MetaTypeType::get(ThisTy, TC.Context);
      if (outerGenericParams) {
        AllocFnTy = PolymorphicFunctionType::get(ThisMetaTy, FnTy,
                                                outerGenericParams, TC.Context);
        InitFnTy = PolymorphicFunctionType::get(ThisTy, FnTy,
                                                outerGenericParams, TC.Context);
      } else {
        AllocFnTy = FunctionType::get(ThisMetaTy, FnTy, TC.Context);
        InitFnTy = FunctionType::get(ThisTy, FnTy, TC.Context);
      }
      CD->setType(AllocFnTy);
      CD->setInitializerType(InitFnTy);
    }

    validateAttributes(CD);
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    if (IsSecondPass)
      return;

    assert(DD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent destructors outside of types!");

    GenericParamList *outerGenericParams = nullptr;
    Type ThisTy = DD->computeThisType(&outerGenericParams);
    TC.validateTypeSimple(ThisTy);
    Type FnTy;
    if (outerGenericParams)
      FnTy = PolymorphicFunctionType::get(ThisTy,
                                          TupleType::getEmpty(TC.Context),
                                          outerGenericParams, TC.Context);
    else
      FnTy = FunctionType::get(ThisTy, TupleType::getEmpty(TC.Context),
                               TC.Context);
    
    DD->setType(FnTy);
    DD->getImplicitThisDecl()->setType(ThisTy);

    validateAttributes(DD);
  }
};
}; // end anonymous namespace.


void TypeChecker::typeCheckDecl(Decl *D, bool isFirstPass) {
  bool isSecondPass = !isFirstPass && D->getDeclContext()->isModuleContext();
  DeclChecker(*this, isFirstPass, isSecondPass).visit(D);
}

ArrayRef<ProtocolDecl *>
TypeChecker::getDirectConformsTo(NominalTypeDecl *nominal) {
  checkInheritanceClause(*this, nominal);
  return nominal->getProtocols();
}

ArrayRef<ProtocolDecl *>
TypeChecker::getDirectConformsTo(ExtensionDecl *ext) {
  checkInheritanceClause(*this, ext);
  return ext->getProtocols();
}

/// \brief Create an implicit struct constructor.
///
/// \param structDecl The struct for which a constructor will be created.
/// \param ICK The kind of implicit constructor to create.
///
/// \returns The newly-created constructor, which has already been type-checked
/// (but has not been added to the containing struct).
static ConstructorDecl *createImplicitConstructor(TypeChecker &tc,
                                                  StructDecl *structDecl,
                                                  ImplicitConstructorKind ICK) {
  ASTContext &context = tc.Context;
  // Determine the parameter type of the implicit constructor.
  SmallVector<TuplePatternElt, 8> patternElts;
  SmallVector<VarDecl *, 8> allArgs;
  if (ICK == ImplicitConstructorKind::Memberwise) {
    for (auto member : structDecl->getMembers()) {
      auto var = dyn_cast<VarDecl>(member);
      if (!var)
        continue;

      // Properties are computed, not initialized.
      if (var->isProperty())
        continue;

      // Create the parameter.
      auto *arg = new (context) VarDecl(SourceLoc(),
                                        var->getName(),
                                        var->getTypeOfRValue(), structDecl);
      allArgs.push_back(arg);
      Pattern *pattern = new (context) NamedPattern(arg);
      TypeLoc tyLoc = TypeLoc::withoutLoc(var->getTypeOfRValue());
      pattern = new (context) TypedPattern(pattern, tyLoc);
      patternElts.push_back(TuplePatternElt(pattern));
    }
  }

  // Crate the onstructor.
  auto constructorID = context.getIdentifier("constructor");
  VarDecl *thisDecl
    = new (context) VarDecl(SourceLoc(),
                            context.getIdentifier("this"),
                            Type(), structDecl);
  ConstructorDecl *ctor
    = new (context) ConstructorDecl(constructorID, structDecl->getLoc(),
                                    nullptr, thisDecl, nullptr, structDecl);
  thisDecl->setDeclContext(ctor);
  for (auto var : allArgs) {
    var->setDeclContext(ctor);
  }
  
  // Set its arguments.
  auto pattern = TuplePattern::create(context, structDecl->getLoc(),
                                      patternElts, structDecl->getLoc());
  ctor->setArguments(pattern);

  // Mark implicit.
  ctor->setImplicit();

  // Type-check the constructor declaration.
  tc.typeCheckDecl(ctor, /*isFirstPass=*/true);

  // If the struct in which this constructor is being added was imported,
  // add it as an external definition.
  if (structDecl->hasClangNode()) {
    tc.Context.ExternalDefinitions.insert(ctor);
  }

  return ctor;
}

void TypeChecker::addImplicitConstructors(StructDecl *structDecl) {
  // Check whether there is a user-declared constructor or,
  // failing that, an instance variable.
  bool FoundConstructor = false;
  bool FoundInstanceVar = false;
  for (auto member : structDecl->getMembers()) {
    if (isa<ConstructorDecl>(member)) {
      FoundConstructor = true;
      break;
    }

    if (auto var = dyn_cast<VarDecl>(member)) {
      if (!var->isProperty())
        FoundInstanceVar = true;
    }
  }

  // If we didn't find such a constructor, add the implicit one(s).
  if (!FoundConstructor) {
    // Copy the list of members, so we can add to it.
    // FIXME: Painfully inefficient to do the copy here.
    SmallVector<Decl *, 4> members(structDecl->getMembers().begin(),
                                   structDecl->getMembers().end());

    // Create the implicit memberwise constructor.
    auto ctor = createImplicitConstructor(*this, structDecl,
                                          ImplicitConstructorKind::Memberwise);
    members.push_back(ctor);

    // Set the members of the struct.
    structDecl->setMembers(Context.AllocateCopy(members),
                           structDecl->getBraces());

    // If we found any instance variables, the default constructor will be
    // different than the memberwise constructor. Whether this
    // constructor will actually be defined depends on whether all of
    // the instance variables can be default-initialized, which we
    // don't know yet. This will be determined lazily.
    if (FoundInstanceVar) {
      assert(!structsNeedingImplicitDefaultConstructor.count(structDecl));
      structsNeedingImplicitDefaultConstructor.insert(structDecl);
      structsWithImplicitDefaultConstructor.push_back(structDecl);
    }
  }
}

bool TypeChecker::isDefaultInitializable(Type ty, Expr **initializer) {
  CanType canTy = ty->getCanonicalType();
  switch (canTy->getKind()) {
  case TypeKind::Archetype:
  case TypeKind::BoundGenericStruct:
  case TypeKind::BoundGenericUnion:
  case TypeKind::Union:
  case TypeKind::Struct:
    // Break to look for constructors.
    break;

  case TypeKind::Array:
    // Arrays are default-initializable if their element types are.
    // FIXME: We don't implement this rule yet, so just fail.
    return false;

  case TypeKind::BoundGenericClass:
  case TypeKind::Class:
    // Classes are default-initializable (with 0).
    // FIXME: This may not be what we want in the long term.
    if (initializer) {
      *initializer = new (Context) ZeroValueExpr(ty);
    }
    return true;

  case TypeKind::Protocol:
  case TypeKind::ProtocolComposition:
    // Existentials are not default-initializable.
    return false;

  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinVector:
    // Built-in types are default-initializable.
    if (initializer) {
      *initializer = new (Context) ZeroValueExpr(ty);
    }
    return true;

  case TypeKind::Tuple: {
    // Check whether all fields either have an initializer or have
    // default-initializable types.
    llvm::SmallVector<Expr *, 4> eltInits;
    llvm::SmallVector<Identifier, 4> eltNames;
    for (auto &elt : ty->castTo<TupleType>()->getFields()) {
      assert(!elt.hasInit() && "Initializers can't appear here");

      // Check whether the element is default-initializable.
      Expr *eltInit = nullptr;
      if (!isDefaultInitializable(elt.getType(),
                                  initializer? &eltInit : nullptr))
        return false;

      // If we need to produce an initializer, add this element.
      if (initializer) {
        assert(eltInit && "Missing initializer?");
        eltInits.push_back(eltInit);
        eltNames.push_back(elt.getName());
      }
    }

    // If we need to build an initializer, build a TupleExpr or use the
    // sole initializer (if all others are unnamed).
    if (initializer) {
      if (eltInits.size() == 1 && eltNames[0].empty())
        *initializer = eltInits[0];
      else
        *initializer
          = new (Context) TupleExpr(SourceLoc(),
                                    Context.AllocateCopy(eltInits),
                                    Context.AllocateCopy(eltNames).data(),
                                    SourceLoc(),
                                    /*hasTrailingClosure=*/false);
    }
    return true;
  }
  
  case TypeKind::Function:
  case TypeKind::LValue:
  case TypeKind::PolymorphicFunction:
  case TypeKind::MetaType:
  case TypeKind::Module:
      return false;

  case TypeKind::ReferenceStorage: {
    auto referent = cast<ReferenceStorageType>(canTy)->getReferentType();
    return isDefaultInitializable(referent, initializer);
  }

  // Sugar types.
#define TYPE(Id, Parent)
#define SUGARED_TYPE(Id, Parent) case TypeKind::Id:
#include "swift/AST/TypeNodes.def"
    llvm_unreachable("Not using the canonical type?");

#define TYPE(Id, Parent)
#define UNCHECKED_TYPE(Id, Parent) case TypeKind::Id:
#include "swift/AST/TypeNodes.def"
    // Error cases.
    return false;
  }

  // We need to look for a default constructor.
  auto ctors = lookupConstructors(ty);
  if (!ctors)
    return false;

  // Check whether we have a constructor that can be called with an empty
  // tuple.
  bool foundDefaultConstructor = false;
  for (auto member : ctors) {
    // Dig out the parameter tuple for this constructor.
    auto ctor = dyn_cast<ConstructorDecl>(member);
    if (!ctor)
      continue;

    auto paramTuple = ctor->getArgumentType()->getAs<TupleType>();
    if (!paramTuple)
      continue;

    // Check whether any of the tuple elements are missing an initializer.
    bool missingInit = false;
    for (auto &elt : paramTuple->getFields()) {
      if (elt.hasInit())
        continue;

      missingInit = true;
      break;
    }
    if (missingInit)
      continue;

    // We found a constructor that can be invoked with an empty tuple.
    if (foundDefaultConstructor) {
      // We found two constructors that can be invoked with an empty tuple.
      return false;
    }

    foundDefaultConstructor = true;
  }

  if (!foundDefaultConstructor || !initializer)
    return foundDefaultConstructor;

  // We found a default constructor. Construct the initializer expression.
  // FIXME: As an optimization, we could build a fully type-checked AST here.
  Expr *arg = new (Context) TupleExpr(SourceLoc(), { }, nullptr, SourceLoc(),
                                      /*hasTrailingClosure=*/false);
  Expr *metatype = new (Context) MetatypeExpr(nullptr, SourceLoc(),
                                              MetaTypeType::get(ty, Context));
  *initializer = new (Context) CallExpr(metatype, arg);

  return true;
}

void TypeChecker::defineDefaultConstructor(StructDecl *structDecl) {
  // Erase this from the set of structs that need an implicit default
  // constructor.
  assert(structsNeedingImplicitDefaultConstructor.count(structDecl));
  structsNeedingImplicitDefaultConstructor.erase(structDecl);

  // Verify that all of the instance variables of this struct have default
  // constructors.
  for (auto member : structDecl->getMembers()) {
    // We only care about pattern bindings.
    auto patternBind = dyn_cast<PatternBindingDecl>(member);
    if (!patternBind)
      continue;

    // If the pattern has an initializer, we don't need any default
    // initialization for its variables.
    if (patternBind->getInit())
      continue;

    // Find the variables in the pattern. They'll each need to be
    // default-initialized.
    SmallVector<VarDecl *, 4> variables;
    patternBind->getPattern()->collectVariables(variables);

    for (auto var : variables) {
      if (var->isProperty())
        continue;

      // If this variable is not default-initializable, we're done: we can't
      // add the default constructor because it will be ill-formed.
      if (!isDefaultInitializable(var->getType(), nullptr))
        return;
    }
  }

  // Create the default constructor.
  auto ctor = createImplicitConstructor(
                *this, structDecl, ImplicitConstructorKind::Default);

  // Copy the list of members, so we can add to it.
  // FIXME: Painfully inefficient to do the copy here.
  SmallVector<Decl *, 4> members(structDecl->getMembers().begin(),
                                 structDecl->getMembers().end());

  // Create the implicit memberwise constructor.
  members.push_back(ctor);

  // Set the members of the struct.
  structDecl->setMembers(Context.AllocateCopy(members),
                         structDecl->getBraces());

  // Create an empty body for the default constructor. The type-check of the
  // constructor body will introduce default initializations of the members.
  ctor->setBody(BraceStmt::create(Context, SourceLoc(), { }, SourceLoc()));

  // Add this to the list of implicitly-defined functions.
  implicitlyDefinedFunctions.push_back(ctor);
}

void TypeChecker::definePendingImplicitDecls() {
  // Default any implicit default constructors.
  for (auto structDecl : structsWithImplicitDefaultConstructor) {
    if (structsNeedingImplicitDefaultConstructor.count(structDecl))
      defineDefaultConstructor(structDecl);
  }
}

void TypeChecker::preCheckProtocol(ProtocolDecl *D) {
  DeclChecker checker(*this, /*isFirstPass=*/true, /*isSecondPass=*/false);
  checkInheritanceClause(*this, D);

  for (auto member : D->getMembers()) {
    if (auto assocType = dyn_cast<TypeAliasDecl>(member)) {
      checkInheritanceClause(*this, assocType);
    }
  }
}

/// validateAttributes - Check that the func/var declaration attributes are ok.
void DeclChecker::validateAttributes(ValueDecl *VD) {
  const DeclAttributes &Attrs = VD->getAttrs();
  Type Ty = VD->getType();
  
  // Get the number of lexical arguments, for semantic checks below.
  int NumArguments = -1;
  FuncDecl *FDOrNull = dyn_cast<FuncDecl>(VD);
  if (FDOrNull) {
    if (AnyFunctionType *FT = Ty->getAs<AnyFunctionType>()) {
      if (FDOrNull->getDeclContext()->isTypeContext() && FDOrNull->isStatic())
        FT = FT->getResult()->castTo<AnyFunctionType>();
      if (TupleType *TT = FT->getInput()->getAs<TupleType>())
        NumArguments = TT->getFields().size();
    }
  }

  bool isOperator = VD->isOperator();

  // Operators must be declared with 'func', not 'var'.
  if (isOperator) {
    if (!FDOrNull) {
      TC.diagnose(VD->getLoc(), diag::operator_not_func);
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  
    // The unary prefix operator '&' is reserved and cannot be overloaded.
    if (FDOrNull->isUnaryOperator() && VD->getName().str() == "&"
        && !Attrs.isPostfix()) {
      TC.diagnose(VD->getStartLoc(), diag::custom_operator_addressof);
      return;
    }
  }

  auto isInClassContext = [](ValueDecl *vd) {
    return bool(vd->getDeclContext()->getDeclaredTypeOfContext()
                  ->getClassOrBoundGenericClass());
  };
  
  if (Attrs.isObjC()) {
    // Only classes, class protocols, instance properties, and methods can be
    // ObjC.
    Optional<Diag<>> error;
    if (isa<ClassDecl>(VD)) {
      /* ok */
    } else if (isa<FuncDecl>(VD) && isInClassContext(VD)) {
      if (isOperator)
        error = diag::invalid_objc_decl;
    } else if (isa<VarDecl>(VD) && isInClassContext(VD)) {
      /* ok */
    } else if (auto *protocol = dyn_cast<ProtocolDecl>(VD)) {
      if (!protocol->requiresClass())
        error = diag::objc_protocol_not_class_protocol;
    } else {
      error = diag::invalid_objc_decl;
    }
    
    if (error) {
      TC.diagnose(VD->getStartLoc(), *error);
      VD->getMutableAttrs().ObjC = false;
      return;
    }
  }

  // Ownership attributes (weak, unowned).
  if (Attrs.hasOwnership()) {
    // Diagnostics expect:
    //   0 - unowned
    //   1 - weak
    assert(unsigned(Attrs.isWeak()) + unsigned(Attrs.isUnowned()) == 1);
    unsigned ownershipKind = (unsigned) Attrs.getOwnership();

    // Only 'var' declarations can have ownership.
    // TODO: captures, consts, etc.
    if (!isa<VarDecl>(VD)) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_ownership_decl,
                  ownershipKind);
      VD->getMutableAttrs().clearOwnership();
      return;
    }

    // Type of declaration must be a reference type.
    if (!VD->getType()->allowsOwnership()) {
      // If we have an opaque type, suggest the possibility of adding
      // a class bound.
      if (VD->getType()->isExistentialType() ||
          VD->getType()->getAs<ArchetypeType>()) {
        TC.diagnose(VD->getStartLoc(), diag::invalid_ownership_opaque_type,
                    ownershipKind, VD->getType());
      } else {
        TC.diagnose(VD->getStartLoc(), diag::invalid_ownership_type,
                    ownershipKind, VD->getType());
      }
      VD->getMutableAttrs().clearOwnership();
      return;
    }

    // Change the type to the appropriate reference storage type.
    VD->overwriteType(ReferenceStorageType::get(VD->getType(),
                                                Attrs.getOwnership(),
                                                TC.Context));
  }

  if (Attrs.isIBOutlet()) {
    // Only instance properties can be IBOutlets.
    // FIXME: This could do some type validation as well (all IBOutlets refer
    // to objects).
    if (!(isa<VarDecl>(VD) && isInClassContext(VD))) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_iboutlet);
      VD->getMutableAttrs().IBOutlet = false;
      return;
    }
  }

  if (Attrs.isIBAction()) {
    // Only instance methods returning () can be IBActions.
    const FuncDecl *FD = dyn_cast<FuncDecl>(VD);
    if (!FD || !isa<ClassDecl>(VD->getDeclContext()) || FD->isStatic() ||
        FD->isGetterOrSetter()) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_ibaction_decl);
      VD->getMutableAttrs().IBAction = false;
      return;
    }

    // IBActions instance methods must have type Class -> (...) -> ().
    // FIXME: This could do some argument type validation as well (only certain
    // method signatures are allowed for IBActions).
    Type CurriedTy = VD->getType()->castTo<AnyFunctionType>()->getResult();
    Type ResultTy = CurriedTy->castTo<AnyFunctionType>()->getResult();
    if (!ResultTy->isEqual(TupleType::getEmpty(TC.Context))) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_ibaction_result, ResultTy);
      VD->getMutableAttrs().IBAction = false;
      return;
    }
  }

  if (Attrs.isInfix()) {
    // Only operator functions can be infix.
    if (!isOperator) {
      TC.diagnose(VD->getStartLoc(), diag::infix_not_an_operator);
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }

    // Only binary operators can be infix.
    if (!FDOrNull || !FDOrNull->isBinaryOperator()) {
      TC.diagnose(Attrs.LSquareLoc, diag::invalid_infix_input);
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  }

  if (Attrs.isPostfix()) {
    // Only operator functions can be postfix.
    if (!isOperator) {
      TC.diagnose(VD->getStartLoc(), diag::postfix_not_an_operator);
      VD->getMutableAttrs().ExplicitPostfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }

    // Only unary operators can be postfix.
    if (!FDOrNull || !FDOrNull->isUnaryOperator()) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_postfix_input);
      VD->getMutableAttrs().ExplicitPostfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  }
  
  if (Attrs.isPrefix()) {
    // Only operator functions can be postfix.
    if (!isOperator) {
      TC.diagnose(VD->getStartLoc(), diag::prefix_not_an_operator);
      VD->getMutableAttrs().ExplicitPostfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
    
    // Only unary operators can be postfix.
    if (!FDOrNull || !FDOrNull->isUnaryOperator()) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_prefix_input);
      VD->getMutableAttrs().ExplicitPostfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  }
  
  if (Attrs.isAssignment()) {
    // Only function declarations can be assignments.
    if (!isa<FuncDecl>(VD) || !VD->isOperator()) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute,"assignment");
      VD->getMutableAttrs().Assignment = false;
    } else if (NumArguments < 1) {
      TC.diagnose(VD->getStartLoc(), diag::assignment_without_byref);
      VD->getMutableAttrs().Assignment = false;
    } else {
      auto FT = VD->getType()->castTo<AnyFunctionType>();
      Type ParamType = FT->getInput();
      TupleType *ParamTT = ParamType->getAs<TupleType>();
      if (ParamTT)
        ParamType = ParamTT->getElementType(0);
      
      if (!ParamType->is<LValueType>()) {
        TC.diagnose(VD->getStartLoc(), diag::assignment_without_byref);
        VD->getMutableAttrs().Assignment = false;
      }
    }
  }

  if (Attrs.isConversion()) {
    // Only instance members with no non-defaulted parameters can be
    // conversions.
    if (!isa<FuncDecl>(VD) || !VD->isInstanceMember()) {
      TC.diagnose(VD->getStartLoc(), diag::conversion_not_instance_method,
                  VD->getName());
      VD->getMutableAttrs().Conversion = false;
    } else if (!VD->getType()->is<ErrorType>()) {
      AnyFunctionType *BoundMethodTy
        = VD->getType()->castTo<AnyFunctionType>()->getResult()
            ->castTo<AnyFunctionType>();
      
      bool AcceptsEmptyParamList = false;
      Type InputTy = BoundMethodTy->getInput();
      if (const TupleType *Tuple = InputTy->getAs<TupleType>()) {
        bool AllDefaulted = true;
        for (auto Elt : Tuple->getFields()) {
          if (!Elt.hasInit()) {
            AllDefaulted = false;
            break;
          }
        }
        
        AcceptsEmptyParamList = AllDefaulted;
      }
      
      if (!AcceptsEmptyParamList) {
        TC.diagnose(VD->getStartLoc(), diag::conversion_params,
                    VD->getName());
        VD->getMutableAttrs().Conversion = false;
      }
    }
  }
  
  if (Attrs.isForceInline()) {
    // Only functions can be force_inline.
    auto *FD = dyn_cast<FuncDecl>(VD);
    if (!FD) {
      TC.diagnose(VD->getStartLoc(), diag::force_inline_not_function);
      VD->getMutableAttrs().ForceInline = false;
    } else if (FD->getBody()->getNumParamPatterns() > 1) {
      // We don't yet support force_inline of curried functions.
      TC.diagnose(VD->getStartLoc(), diag::force_inline_curry_not_supported);
      VD->getMutableAttrs().ForceInline = false;
    } else if (FD->getGenericParams()) {
      // We don't yet support force_inline of generic functions.
      TC.diagnose(VD->getStartLoc(), diag::force_inline_generic_not_supported);
      VD->getMutableAttrs().ForceInline = false;      
    }
  }
  
  if (Attrs.isByref()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "byref");
    VD->getMutableAttrs().Byref = false;
  }

  if (Attrs.isAutoClosure()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "auto_closure");
    VD->getMutableAttrs().AutoClosure = false;
  }

  if (Attrs.isObjCBlock()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "objc_block");
    VD->getMutableAttrs().ObjCBlock = false;
  }

  // Only protocols can have the [class_protocol] attribute.
  if (Attrs.isClassProtocol() && !isa<ProtocolDecl>(VD)) {
    TC.diagnose(VD->getStartLoc(), diag::class_protocol_not_protocol);
    VD->getMutableAttrs().ClassProtocol = false;
  }
  
  if (Attrs.hasCC()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "cc");
    VD->getMutableAttrs().cc = {};
  }

  if (Attrs.isThin()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "thin");
    VD->getMutableAttrs().Thin = false;
  }

  if (Attrs.isNoReturn()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "noreturn");
    VD->getMutableAttrs().NoReturn = false;
  }

}
