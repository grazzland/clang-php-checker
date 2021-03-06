//===-- PHPZPPChecker.cpp -------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Defines a checker for proper use of PHP's zend_parse_parameters(),
// zend_parse_parameters(), zend_parse_method_parameters() and
// zend_parse_method_parameters_ex() functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringSwitch.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerRegistry.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"

#include <cassert>

using namespace clang;
using namespace ento;

namespace {
class PHPNativeType {
  StringRef name;
  bool hasVal;
  int pointerLevel;

public:
  PHPNativeType() : hasVal(false), pointerLevel(0) {}
  PHPNativeType(StringRef name, int pointerLevel = 0)
      : name(name), hasVal(true), pointerLevel(pointerLevel) {}

  PHPNativeType(StringRef name, const char *pointerLevel)
      : name(name), hasVal(true), pointerLevel(strlen(pointerLevel)) {}

  StringRef getName() const {
    assert(hasVal);
    return name;
  }

  int getPointerLevel() const { return pointerLevel; }

  operator bool() const { return hasVal; }
};

typedef std::vector<PHPNativeType> PHPTypeVector;
typedef std::map<char, PHPTypeVector > PHPTypeMap;

PHPTypeVector &operator<<(PHPTypeMap &map, char format) {
  return map.insert(PHPTypeMap::value_type(
                        format, PHPTypeVector())).first->second;
}

PHPTypeVector &operator&(PHPTypeVector &vec,
                                       const StringRef &name) {
  std::pair<StringRef, StringRef> type = name.split(' ');
  vec.push_back(PHPNativeType(type.first, type.second.size()));
  return vec;
}

// These mappings map a zpp modifier to underlying types.
// Mind: zpp receives the address of the object to store in wich adds an
// indirection level.
// Some types return multiple values, these are added multiple times in order to
// this list (i.e. a string "s" consists of a char array and length)
static void fillMapPHPBase(PHPTypeMap &map) {
  map << 'a' & "zval **";
  map << 'A' & "zval **";
  map << 'b' & "zend_bool *";
  map << 'C' & "zend_class_entry **";
  map << 'd' & "double *";
  map << 'f' & "zend_fcall_info *"
             & "zend_fcall_info_cache *";
  map << 'h' & "HashTable **";
  map << 'H' & "HashTable **";
  map << 'o' & "zval **";
  map << 'O' & "zval **"
             & "zend_class_entry *";
  map << 'r' & "zval **";
  map << 'z' & "zval **";
  map << '|';
  map << '/';
  map << '!';
  map << '+' & "zval ****"
             & "int *";
  map << '*' & "zval ****"
             & "int *";
}

static void fillMapPHP5(PHPTypeMap &map) {
  fillMapPHPBase(map);
  map << 'l' & "long *";
  map << 'L' & "long *";
  map << 'p' & "char **"
             & "int *";
  map << 's' & "char **"
             & "int *";
  map << 'Z' & "zval ***";
}

static void fillMapPHP7(PHPTypeMap &map) {
  fillMapPHPBase(map);
  map << 'l' & "zend_long *";
  map << 'L' & "zend_long *";
  map << 'P' & "zend_string **";
  map << 'S' & "zend_string **";
  map << 'p' & "char **"
             & "size_t *";
  map << 's' & "char **"
             & "size_t *";
}

class PHPZPPChecker
    : public Checker<check::PreCall, check::ASTDecl<TypedefDecl> > {
  mutable IdentifierInfo *IIzpp, *IIzpp_ex, *IIzpmp, *IIzpmp_ex;

#if (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 5) ||                  \
    CLANG_VERSION_MAJOR > 3
  std::unique_ptr<BugType> InvalidTypeBugType;
  std::unique_ptr<BugType> InvalidModifierBugType;
  std::unique_ptr<BugType> WrongArgumentNumberBugType;
#else
  OwningPtr<BugType> InvalidTypeBugType;
  OwningPtr<BugType> InvalidModifierBugType;
  OwningPtr<BugType> WrongArgumentNumberBugType;
#endif
  PHPTypeMap map;

  typedef std::map<const StringRef, const QualType> TypedefMap;
  mutable TypedefMap typedefs;

  void initIdentifierInfo(ASTContext &Ctx) const;
  const StringLiteral *getCStringLiteral(const SVal &val) const;

  void reportInvalidType(unsigned offset, char modifier, const SVal &val,
                         const PHPNativeType &expectedType,
                         const QualType initialType, CheckerContext &C) const;

  void reportInvalidIndirection(unsigned offset, char modifier, const SVal &val,
                                const PHPNativeType &expectedType,
                                int passedPointerLevel,
                                CheckerContext &C) const;

  void reportUnknownModifier(char modifier, CheckerContext &C) const;

  void reportTooFewArgs(const StringRef &format_spec, char modifier,
                        CheckerContext &C) const;

  void reportTooManyArgs(const StringRef &format_spec, unsigned min_req,
                         unsigned offset, const CallEvent &Call,
                         CheckerContext &C) const;

  void compareTypeWithSVal(unsigned offset, char modifier, const SVal &val,
                           const PHPNativeType &expectedType,
                           CheckerContext &C) const;
  bool checkArgs(const StringRef &format_spec, unsigned &offset,
                 const unsigned numArgs, const CallEvent &Call,
                 CheckerContext &C) const;

public:
  PHPZPPChecker();

  typedef void (*MapFiller)(PHPTypeMap &);
  void setMap(MapFiller filler);
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkASTDecl(const TypedefDecl *td, AnalysisManager &Mgr,
                    BugReporter &BR) const;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const PHPNativeType &type) {
  os << type.getName() << " ";
  for (int i = 0; i < type.getPointerLevel(); ++i) {
    os << "*";
  }
  return os;
}

}

static raw_ostream &debug_stream() {
#ifdef DEBUG_PHP_ZPP_CHECKER
  return llvm::outs();
#else
  return llvm::nulls();
#endif
}

static BugType *createZZPAPIError(StringRef name) {
  return new BugType(
#if (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 5) ||                  \
    CLANG_VERSION_MAJOR > 3
      new CheckerBase(),
#endif
      name, "PHP ZPP API Error");
}

PHPZPPChecker::PHPZPPChecker()
    : IIzpp(0), IIzpp_ex(0), IIzpmp(0), IIzpmp_ex(0) {
  InvalidTypeBugType.reset(createZZPAPIError("Invalid type"));

  InvalidModifierBugType.reset(createZZPAPIError("Invalid modifier"));

  WrongArgumentNumberBugType.reset(createZZPAPIError("Wrong number of zpp arguments"));
}

void PHPZPPChecker::setMap(MapFiller filler) {
  map.clear();
  filler(map);
}

const StringLiteral *PHPZPPChecker::getCStringLiteral(const SVal &val) const {

  // Copied from tools/clang/lib/StaticAnalyzer/Checkers/CStringChecker.cpp

  // Get the memory region pointed to by the val.
  const MemRegion *bufRegion = val.getAsRegion();
  if (!bufRegion)
    return NULL;

  // Strip casts off the memory region.
  bufRegion = bufRegion->StripCasts();

  // Cast the memory region to a string region.
  const StringRegion *strRegion = dyn_cast<StringRegion>(bufRegion);
  if (!strRegion)
    return NULL;

  // Return the actual string in the string region.
  return strRegion->getStringLiteral();
}

void PHPZPPChecker::reportInvalidType(unsigned offset, char modifier,
                                      const SVal &val,
                                      const PHPNativeType &expectedType,
                                      const QualType initialType,
                                      CheckerContext &C) const {
  SmallString<256> buf;
  llvm::raw_svector_ostream os(buf);
  os << "Type of passed argument ";
  val.dumpToStream(os);
  os << " is of type " << initialType.getAsString()
     << " which did not match expected " << expectedType << " for modifier '"
     << modifier << "' at offset " << offset + 1 << ".";
  BugReport *R =
      new BugReport(*InvalidTypeBugType, os.str(), C.addTransition());
  R->markInteresting(val);
  C.emitReport(R);
}

void PHPZPPChecker::reportInvalidIndirection(unsigned offset, char modifier,
                                             const SVal &val,
                                             const PHPNativeType &expectedType,
                                             int passedPointerLevel,
                                             CheckerContext &C) const {
  SmallString<256> buf;
  llvm::raw_svector_ostream os(buf);
  os << "Pointer indirection level of passed argument ";
  val.dumpToStream(os);
  os << " is " << passedPointerLevel << " which did not match expected "
     << expectedType.getPointerLevel() << " of " << expectedType
     << " for modifier '" << modifier << "' at offset " << offset + 1 << ".";
  BugReport *R =
      new BugReport(*InvalidTypeBugType, os.str(), C.addTransition());
  R->markInteresting(val);
  C.emitReport(R);
}

void PHPZPPChecker::reportTooFewArgs(const StringRef &format_spec,
                                     char modifier, CheckerContext &C) const {
  SmallString<255> buf;
  llvm::raw_svector_ostream os(buf);
  os << "Too few arguments for format \"" << format_spec
     << "\" while checking for modifier '" << modifier << "'.";
  BugReport *R =
      new BugReport(*WrongArgumentNumberBugType, os.str(), C.addTransition());
  C.emitReport(R);
}

void PHPZPPChecker::reportUnknownModifier(char modifier,
                                          CheckerContext &C) const {
  SmallString<32> buf;
  llvm::raw_svector_ostream os(buf);
  os << "Unknown modifier '" << modifier << "'";
  BugReport *R =
      new BugReport(*InvalidModifierBugType, os.str(), C.addTransition());
  C.emitReport(R);
}

void PHPZPPChecker::reportTooManyArgs(const StringRef &format_spec,
                                      unsigned min_req, unsigned offset,
                                      const CallEvent &Call,
                                      CheckerContext &C) const {
  SmallString<32> buf;
  llvm::raw_svector_ostream os(buf);
  os << "Too many arguments, modifier \"" << format_spec << "\" requires "
     << offset - min_req + 1 << " arguments.";
  BugReport *R =
      new BugReport(*WrongArgumentNumberBugType, os.str(), C.addTransition());
  R->markInteresting(Call.getArgSVal(offset));
  C.emitReport(R);
}

static const QualType getQualTypeForSVal(const SVal &val) {
  const MemRegion *region = val.getAsRegion();
  if (!region) {
    // TODO: pdo_dbh.c uses zpp(0 TSRMLS_CC, "z|z", NULL, NULL) to report an error
    // Should we report an error for such a construct? In other situations it could be wrong
    // For this specific case we could mitigate by checking whether the first argument (ht)
    // is 0 as it is hard coded in this case. Right now we report no error if NULL is passed.
    return QualType();
  }

  if (isa<TypedValueRegion>(region)) {
    return cast<TypedValueRegion>(region)->getLocationType();
  }
  if (isa<SymbolicRegion>(region)) {
    return cast<SymbolicRegion>(region)
        ->getSymbol()
        ->getType();
  }
  return QualType();
}

void PHPZPPChecker::compareTypeWithSVal(unsigned offset, char modifier, const SVal &val,
                                            const PHPNativeType &expectedType,
                                            CheckerContext &C) const {
 
  const QualType initialType = getQualTypeForSVal(val);

  if (initialType.isNull()) {
    // TODO need a good way to report this, even though this is no error
    llvm::outs() << "Couldn't get type for argument at offset " << offset << "\n";
    val.dump();
    return;
  }

  QualType type = initialType.getCanonicalType();

  int passedPointerLevel = 0;
  while (type->isPointerType()) {
    ++passedPointerLevel;
    type = type->getPointeeType();
  }

  bool match = false;
  const TypedefMap::const_iterator typedef_ = typedefs.find(expectedType.getName());
  if (typedef_ != typedefs.end()) {
    match = (type == typedef_->second.getCanonicalType());
  } else {
    match = (type.getAsString() == expectedType.getName());
  }

  if (!match) {
    reportInvalidType(offset, modifier, val, expectedType, initialType, C);
    return;
  }

  if (passedPointerLevel != expectedType.getPointerLevel()) {
    reportInvalidIndirection(offset, modifier, val, expectedType, passedPointerLevel, C);
    return;
  }

  return;
}

bool PHPZPPChecker::checkArgs(const StringRef &format_spec, unsigned &offset,
                              const unsigned numArgs, const CallEvent &Call,
                              CheckerContext &C) const {
  Call.dump(debug_stream());
  for (StringRef::const_iterator modifier = format_spec.begin(),
                                 last_mod = format_spec.end();
       modifier != last_mod; ++modifier) {
    debug_stream() << "  I am checking for " << *modifier << "\n";
    const PHPTypeMap::const_iterator it = map.find(*modifier);

    if (it == map.end()) {
      reportUnknownModifier(*modifier, C);
      return false;
    }

    for (PHPTypeVector::const_iterator type = it->second.begin();
         type != it->second.end(); ++type) {
      ++offset;
      debug_stream() << "    I need a " << *type << " (" << offset << ")\n";
      if (numArgs <= offset) {
        reportTooFewArgs(format_spec, *modifier, C);
        debug_stream() << "!!!!I am missing args! " << numArgs << "<=" << offset << "\n";
        return false;
      }

      const SVal val = Call.getArgSVal(offset);
      compareTypeWithSVal(offset, *modifier, val, *type, C);
      // Even if there is a type mismatch we can continue, most of the time
      // this should be a simple mistake by the user, in rare cases the user
      // missed an argument and will get many subsequent errors
    }
  }
  return true;
}

void PHPZPPChecker::checkPreCall(const CallEvent &Call,
                                 CheckerContext &C) const {
  initIdentifierInfo(C.getASTContext());

  if (!Call.isGlobalCFunction())
    return;

  const IdentifierInfo *callee = Call.getCalleeIdentifier();
  if (callee != IIzpp && callee && IIzpp_ex && callee != IIzpmp &&
      callee != IIzpmp_ex) {
    return;
  }

  const FunctionDecl *decl = cast<FunctionDecl>(Call.getDecl());
  // we want the offset to be the last required argument which is the format
  // string, offset is 0-based, thus -1
  unsigned offset = decl->getMinRequiredArguments() - 1;

  const unsigned numArgs = Call.getNumArgs();
  if (numArgs <= offset)
    // Something is really weird - this should be caught by the compiler
    return;

  const StringLiteral *format_spec_sl =
      getCStringLiteral(Call.getArgSVal(offset));
  if (!format_spec_sl) {
    // TODO need a good way to report this, even though this is no error
    llvm::outs() << "Couldn't get format string looked at offset " << offset << "\n";
    Call.dump();
    return;
  }
  const StringRef format_spec = format_spec_sl->getBytes();

  // All preconditions met, we can do the actual check \o/
  if (!checkArgs(format_spec, offset, numArgs, Call, C))
    return;

  if (numArgs > 1 + offset) {
    reportTooManyArgs(format_spec, decl->getMinRequiredArguments(), offset, Call, C);
  }
}

void PHPZPPChecker::checkASTDecl(const TypedefDecl *td, AnalysisManager &Mgr, BugReporter &BR) const {
  // This extra map might be quite inefficient, probably we should iterat over the delarations, later in
  // the check?
  // for (DeclContext::decl_iterator it = decl->getParent()->decls_begin(); it != decl->getParent()->decls_end(); ++it) { if (isa<TypedefDecl>(*it)) {

  typedefs.insert(TypedefMap::value_type(td->getName(), td->getUnderlyingType()));
}

void PHPZPPChecker::initIdentifierInfo(ASTContext &Ctx) const {
  if (IIzpp)
    return;

  IIzpp = &Ctx.Idents.get("zend_parse_parameters");
  IIzpp_ex = &Ctx.Idents.get("zend_parse_parameters_ex");
  IIzpmp = &Ctx.Idents.get("zend_parse_method_parameters");
  IIzpmp_ex = &Ctx.Idents.get("zend_parse_method_parameters_ex");
}

static void initPHPChecker(CheckerManager &mgr) {
  const char *version = getenv("PHP_ZPP_CHECKER_VERSION");
  if (!version) {
    version = "PHP5";
  }
  PHPZPPChecker *checker = mgr.registerChecker<PHPZPPChecker>();
  PHPZPPChecker::MapFiller filler =
      llvm::StringSwitch<PHPZPPChecker::MapFiller>(
          mgr.getAnalyzerOptions()
              .Config.GetOrCreateValue("php-zpp-version", version)
              .getValue())
          .Case("PHP5", fillMapPHP5)
          .Case("PHP7", fillMapPHP7)
          .Default(NULL);
  if (filler) {
    checker->setMap(filler);
  }
}


extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker(initPHPChecker, "php.ZPPChecker",
                      "Check zend_parse_parameters usage");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
