#include "GoogleTest/GoogleTestRunner.h"

#include "GoogleTest/GoogleTest_Test.h"

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/OrcMCJITReplacement.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"

#include <chrono>
#include <execinfo.h>

using namespace mull;
using namespace llvm;
using namespace std::chrono;
using namespace llvm::orc;

namespace {
  class UnitTest;
}

typedef void (*mull_destructor_t)(void *);

struct atexit_entry {
  mull_destructor_t destructor;
  void *arg;
  void *dso_handle;
};

const static int dtors_count = 64;
static int current_dtor = 0;
static atexit_entry dtors[dtors_count];

extern "C" int mull__cxa_atexit(mull_destructor_t destructor, void *arg, void *__dso_handle) {
  assert(current_dtor < dtors_count);

#if 0
  void* callstack[128];
  int i, frames = backtrace(callstack, 128);
  char** strs = backtrace_symbols(callstack, frames);
  for (i = 0; i < frames; ++i) {
    printf("%s\n", strs[i]);
  }
  free(strs);
#endif

  for (int i = 0; i < current_dtor; i++) {
    if (arg == dtors[i].arg) {
//      printf("dtor already registered: %d: %p\n", i, arg);
      return 0;
    }
  }

//  printf("record dtor: %d: %p\n", current_dtor, arg);

  dtors[current_dtor].destructor = destructor;
  dtors[current_dtor].arg = arg;
  dtors[current_dtor].dso_handle = __dso_handle;

  current_dtor++;

  return 0;
}

void runDestructors() {
//  printf("dtors: %d\n", current_dtor);
  while (current_dtor > 0) {
    current_dtor--;
//    printf("cleaning dtor: %d: %p\n", current_dtor, dtors[current_dtor].arg);
    dtors[current_dtor].destructor(dtors[current_dtor].arg);
  }
}

/// Hijacking output functions to prevent extra logging

extern "C" int mull_vprintf(const char *restrict, va_list) {
  return 0;
}

extern "C" int mull_printf(const char *fmt, ...) {
  /// ignoring
  return 0;
}

extern "C" void *mull__dso_handle = nullptr;

/// We use LLVM Mangler class for low-level mangling: '_' prefixing.
/// Examples:
/// Mac OS:
/// _ZN7testing14InitGoogleTestEPiPPc -> __ZN7testing14InitGoogleTestEPiPPc
/// On Linux it has no effect:
/// _ZN7testing14InitGoogleTestEPiPPc -> _ZN7testing14InitGoogleTestEPiPPc
/// TODO: extract it to a separate class.
/// TODO: remove braces?
static std::string getNameWithPrefix(const std::string &name,
                                     const llvm::DataLayout &dataLayout) {
  const llvm::StringRef &stringRefName = name;
  std::string MangledName;
  {
    raw_string_ostream Stream(MangledName);
    llvm::Mangler::getNameWithPrefix(Stream,
                                     stringRefName,
                                     dataLayout);
  }
  return MangledName;
}

class Mull_GoogleTest_Resolver : public RuntimeDyld::SymbolResolver {

  std::map<std::string, std::string> mapping;

public:

  Mull_GoogleTest_Resolver(std::map<std::string, std::string> mapping)
    : mapping(mapping) {}

  RuntimeDyld::SymbolInfo findSymbol(const std::string &name) {
    if (mapping.count(name) > 0) {
      return findSymbol(mapping[name]);
    }

    if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(name))
      return RuntimeDyld::SymbolInfo(SymAddr, JITSymbolFlags::Exported);

    return RuntimeDyld::SymbolInfo(nullptr);
  }

  RuntimeDyld::SymbolInfo findSymbolInLogicalDylib(const std::string &name) {
    return RuntimeDyld::SymbolInfo(nullptr);   }
};

GoogleTestRunner::GoogleTestRunner(llvm::TargetMachine &machine)
  : TestRunner(machine) {
  // TODO: Would be create to not have all of the following here.
  // Some builder class?
  DataLayout dataLayout = machine.createDataLayout();

  std::string atExitFunction = getNameWithPrefix("__cxa_atexit", dataLayout);
  std::string dsoHandleFunction = getNameWithPrefix("__dso_handle", dataLayout);
  mapping[atExitFunction] = "mull__cxa_atexit";
  mapping[dsoHandleFunction] = "mull__dso_handle";
  this->mapping = mapping;

  fGoogleTestInit.assign(
    getNameWithPrefix("_ZN7testing14InitGoogleTestEPiPPc", dataLayout)
  );
  fGoogleTestInstance.assign(
    getNameWithPrefix("_ZN7testing8UnitTest11GetInstanceEv", dataLayout)
  );
  fGoogleTestRun.assign(
    getNameWithPrefix("_ZN7testing8UnitTest3RunEv", dataLayout)
  );
}

void *GoogleTestRunner::GetCtorPointer(const llvm::Function &Function) {
  return getFunctionPointer(getNameWithPrefix(Function.getName().str(),
                                              machine.createDataLayout()));
}

void *GoogleTestRunner::getFunctionPointer(const std::string &functionName) {
  JITSymbol symbol = ObjectLayer.findSymbol(functionName, false);

  void *fpointer =
    reinterpret_cast<void *>(static_cast<uintptr_t>(symbol.getAddress()));

  if (fpointer == nullptr) {
    errs() << "GoogleTestRunner> Can't find pointer to function: "
           << functionName << "\n";
    exit(1);
  }

  return fpointer;
}

void GoogleTestRunner::runStaticCtor(llvm::Function *Ctor) {
//  printf("Init: %s\n", Ctor->getName().str().c_str());

  void *CtorPointer = GetCtorPointer(*Ctor);

  auto ctor = ((int (*)())(intptr_t)CtorPointer);
  ctor();
}

ExecutionResult GoogleTestRunner::runTest(Test *Test, ObjectFiles &ObjectFiles) {
  GoogleTest_Test *GTest = dyn_cast<GoogleTest_Test>(Test);

  auto Handle =
    ObjectLayer.addObjectSet(ObjectFiles,
                             make_unique<SectionMemoryManager>(),
                             make_unique<Mull_GoogleTest_Resolver>(this->mapping));

  auto start = high_resolution_clock::now();

  for (auto &Ctor: GTest->GetGlobalCtors()) {
    runStaticCtor(Ctor);
  }

  std::string filter = "--gtest_filter=" + GTest->getTestName();
  const char *argv[] = { "mull", filter.c_str(), NULL };
  int argc = 2;

  /// Normally Google Test Driver looks like this:
  ///
  ///   int main(int argc, char **argv) {
  ///     InitGoogleTest(&argc, argv);
  ///     return UnitTest.GetInstance()->Run();
  ///   }
  ///
  /// Technically we can just call `main` function, but there is a problem:
  /// Among all the files that are being processed may be more than one
  /// `main` function, therefore we can call wrong driver.
  ///
  /// To avoid this from happening we implement the driver function on our own.
  /// We must keep in mind that each project can have its own, extended
  /// version of the driver (LLVM itself has one).
  ///

  void *initGTestPtr = getFunctionPointer(fGoogleTestInit);

  auto initGTest = ((void (*)(int *, const char**))(intptr_t)initGTestPtr);
  initGTest(&argc, argv);

  void *getInstancePtr = getFunctionPointer(fGoogleTestInstance);

  auto getInstance = ((UnitTest *(*)())(intptr_t)getInstancePtr);
  UnitTest *test = getInstance();

  void *runAllTestsPtr = getFunctionPointer(fGoogleTestRun);

  auto runAllTests = ((int (*)(UnitTest *))(intptr_t)runAllTestsPtr);
  uint64_t result = runAllTests(test);

  runDestructors();
  auto elapsed = high_resolution_clock::now() - start;

  //printf("%llu %s\n", result, GTest->getTestName().c_str());

  ExecutionResult Result;
  Result.RunningTime = duration_cast<std::chrono::milliseconds>(elapsed).count();

  ObjectLayer.removeObjectSet(Handle);

  if (result == 0) {
    Result.Status = ExecutionStatus::Passed;
  } else {
    Result.Status = ExecutionStatus::Failed;
  }

  return Result;
}
