/*
 * Copyright 2010-2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bcc/Compiler.h"

#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/CodeGen/RegAllocRegistry.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Target/TargetSubtargetInfo.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Vectorize.h>

#include "bcc/Assert.h"
#include "bcc/Renderscript/RSScript.h"
#include "bcc/Renderscript/RSTransforms.h"
#include "bcc/Script.h"
#include "bcc/Source.h"
#include "bcc/Support/CompilerConfig.h"
#include "bcc/Support/Log.h"
#include "bcc/Support/OutputFile.h"
#include "bcinfo/MetadataExtractor.h"
#include "rsDefines.h"

#include <string>

using namespace bcc;

const char *Compiler::GetErrorString(enum ErrorCode pErrCode) {
  switch (pErrCode) {
  case kSuccess:
    return "Successfully compiled.";
  case kInvalidConfigNoTarget:
    return "Invalid compiler config supplied (getTarget() returns nullptr.) "
           "(missing call to CompilerConfig::initialize()?)";
  case kErrCreateTargetMachine:
    return "Failed to create llvm::TargetMachine.";
  case kErrSwitchTargetMachine:
    return  "Failed to switch llvm::TargetMachine.";
  case kErrNoTargetMachine:
    return "Failed to compile the script since there's no available "
           "TargetMachine. (missing call to Compiler::config()?)";
  case kErrMaterialization:
    return "Failed to materialize the module.";
  case kErrInvalidOutputFileState:
    return "Supplied output file was invalid (in the error state.)";
  case kErrPrepareOutput:
    return "Failed to prepare file for output.";
  case kPrepareCodeGenPass:
    return "Failed to construct pass list for code-generation.";
  case kErrCustomPasses:
    return "Error occurred while adding custom passes.";
  case kErrInvalidSource:
    return "Error loading input bitcode";
  case kIllegalGlobalFunction:
    return "Use of undefined external function";
  }

  // This assert should never be reached as the compiler verifies that the
  // above switch coveres all enum values.
  assert(false && "Unknown error code encountered");
  return  "";
}

//===----------------------------------------------------------------------===//
// Instance Methods
//===----------------------------------------------------------------------===//
Compiler::Compiler() : mTarget(nullptr), mEnableOpt(true) {
  return;
}

Compiler::Compiler(const CompilerConfig &pConfig) : mTarget(nullptr),
                                                    mEnableOpt(true) {
  const std::string &triple = pConfig.getTriple();

  enum ErrorCode err = config(pConfig);
  if (err != kSuccess) {
    ALOGE("%s (%s, features: %s)", GetErrorString(err),
          triple.c_str(), pConfig.getFeatureString().c_str());
    return;
  }

  return;
}

enum Compiler::ErrorCode Compiler::config(const CompilerConfig &pConfig) {
  if (pConfig.getTarget() == nullptr) {
    return kInvalidConfigNoTarget;
  }

  llvm::TargetMachine *new_target =
      (pConfig.getTarget())->createTargetMachine(pConfig.getTriple(),
                                                 pConfig.getCPU(),
                                                 pConfig.getFeatureString(),
                                                 pConfig.getTargetOptions(),
                                                 pConfig.getRelocationModel(),
                                                 pConfig.getCodeModel(),
                                                 pConfig.getOptimizationLevel());

  if (new_target == nullptr) {
    return ((mTarget != nullptr) ? kErrSwitchTargetMachine :
                                   kErrCreateTargetMachine);
  }

  // Replace the old TargetMachine.
  delete mTarget;
  mTarget = new_target;

  // Adjust register allocation policy according to the optimization level.
  //  createFastRegisterAllocator: fast but bad quality
  //  createLinearScanRegisterAllocator: not so fast but good quality
  if ((pConfig.getOptimizationLevel() == llvm::CodeGenOpt::None)) {
    llvm::RegisterRegAlloc::setDefault(llvm::createFastRegisterAllocator);
  } else {
    llvm::RegisterRegAlloc::setDefault(llvm::createGreedyRegisterAllocator);
  }

  return kSuccess;
}

Compiler::~Compiler() {
  delete mTarget;
}


enum Compiler::ErrorCode Compiler::runPasses(Script &pScript,
                                             llvm::raw_pwrite_stream &pResult) {
  // Pass manager for link-time optimization
  llvm::legacy::PassManager passes;

  // Empty MCContext.
  llvm::MCContext *mc_context = nullptr;

  passes.add(createTargetTransformInfoWrapperPass(mTarget->getTargetIRAnalysis()));

  // Add our custom passes.
  if (!addCustomPasses(pScript, passes)) {
    return kErrCustomPasses;
  }

  if (mTarget->getOptLevel() == llvm::CodeGenOpt::None) {
    passes.add(llvm::createGlobalOptimizerPass());
    passes.add(llvm::createConstantMergePass());

  } else {
    // FIXME: Figure out which passes should be executed.
    llvm::PassManagerBuilder Builder;
    Builder.Inliner = llvm::createFunctionInliningPass();
    Builder.populateLTOPassManager(passes);

    /* FIXME: Reenable autovectorization after rebase.
       bug 19324423
    // Add vectorization passes after LTO passes are in
    // additional flag: -unroll-runtime
    passes.add(llvm::createLoopUnrollPass(-1, 16, 0, 1));
    // Need to pass appropriate flags here: -scalarize-load-store
    passes.add(llvm::createScalarizerPass());
    passes.add(llvm::createCFGSimplificationPass());
    passes.add(llvm::createScopedNoAliasAAPass());
    passes.add(llvm::createScalarEvolutionAliasAnalysisPass());
    // additional flags: -slp-vectorize-hor -slp-vectorize-hor-store (unnecessary?)
    passes.add(llvm::createSLPVectorizerPass());
    passes.add(llvm::createDeadCodeEliminationPass());
    passes.add(llvm::createInstructionCombiningPass());
    */
  }

  // These passes have to come after LTO, since we don't want to examine
  // functions that are never actually called.
  if (!addPostLTOCustomPasses(passes)) {
    return kErrCustomPasses;
  }

  // RSEmbedInfoPass needs to come after we have scanned for non-threadable
  // functions.
  // Script passed to RSCompiler must be a RSScript.
  RSScript &script = static_cast<RSScript &>(pScript);
  if (script.getEmbedInfo())
    passes.add(createRSEmbedInfoPass());

  // Add passes to the pass manager to emit machine code through MC layer.
  if (mTarget->addPassesToEmitMC(passes, mc_context, pResult,
                                 /* DisableVerify */false)) {
    return kPrepareCodeGenPass;
  }

  // Execute the passes.
  passes.run(pScript.getSource().getModule());

  return kSuccess;
}

enum Compiler::ErrorCode Compiler::compile(Script &pScript,
                                           llvm::raw_pwrite_stream &pResult,
                                           llvm::raw_ostream *IRStream) {
  llvm::Module &module = pScript.getSource().getModule();
  enum ErrorCode err;

  if (mTarget == nullptr) {
    return kErrNoTargetMachine;
  }

  const std::string &triple = module.getTargetTriple();
  const llvm::DataLayout *dl = getTargetMachine().getDataLayout();
  unsigned int pointerSize = dl->getPointerSizeInBits();
  if (triple == "armv7-none-linux-gnueabi") {
    if (pointerSize != 32) {
      return kErrInvalidSource;
    }
  } else if (triple == "aarch64-none-linux-gnueabi") {
    if (pointerSize != 64) {
      return kErrInvalidSource;
    }
  } else {
    return kErrInvalidSource;
  }

  // Materialize the bitcode module.
  if (module.getMaterializer() != nullptr) {
    // A module with non-null materializer means that it is a lazy-load module.
    // Materialize it now via invoking MaterializeAllPermanently(). This
    // function returns false when the materialization is successful.
    std::error_code ec = module.materializeAllPermanently();
    if (ec) {
      ALOGE("Failed to materialize the module `%s'! (%s)",
            module.getModuleIdentifier().c_str(), ec.message().c_str());
      return kErrMaterialization;
    }
  }

  if ((err = runPasses(pScript, pResult)) != kSuccess) {
    return err;
  }

  if (IRStream) {
    *IRStream << module;
  }

  return kSuccess;
}

enum Compiler::ErrorCode Compiler::compile(Script &pScript,
                                           OutputFile &pResult,
                                           llvm::raw_ostream *IRStream) {
  // Check the state of the specified output file.
  if (pResult.hasError()) {
    return kErrInvalidOutputFileState;
  }

  // Open the output file decorated in llvm::raw_ostream.
  llvm::raw_pwrite_stream *out = pResult.dup();
  if (out == nullptr) {
    return kErrPrepareOutput;
  }

  // Delegate the request.
  enum Compiler::ErrorCode err = compile(pScript, *out, IRStream);

  // Close the output before return.
  delete out;

  return err;
}

bool Compiler::addInternalizeSymbolsPass(Script &pScript, llvm::legacy::PassManager &pPM) {
  // Add a pass to internalize the symbols that don't need to have global
  // visibility.
  RSScript &script = static_cast<RSScript &>(pScript);
  llvm::Module &module = script.getSource().getModule();
  bcinfo::MetadataExtractor me(&module);
  if (!me.extract()) {
    bccAssert(false && "Could not extract metadata for module!");
    return false;
  }

  // The vector contains the symbols that should not be internalized.
  std::vector<const char *> export_symbols;

  const char *sf[] = {
    kRoot,               // Graphics drawing function or compute kernel.
    kInit,               // Initialization routine called implicitly on startup.
    kRsDtor,             // Static global destructor for a script instance.
    kRsInfo,             // Variable containing string of RS metadata info.
    kRsGlobalEntries,    // Optional number of global variables.
    kRsGlobalNames,      // Optional global variable name info.
    kRsGlobalAddresses,  // Optional global variable address info.
    kRsGlobalSizes,      // Optional global variable size info.
    kRsGlobalProperties, // Optional global variable properties.
    nullptr              // Must be nullptr-terminated.
  };
  const char **special_functions = sf;
  // Special RS functions should always be global symbols.
  while (*special_functions != nullptr) {
    export_symbols.push_back(*special_functions);
    special_functions++;
  }

  // Visibility of symbols appeared in rs_export_var and rs_export_func should
  // also be preserved.
  size_t exportVarCount = me.getExportVarCount();
  size_t exportFuncCount = me.getExportFuncCount();
  size_t exportForEachCount = me.getExportForEachSignatureCount();
  const char **exportVarNameList = me.getExportVarNameList();
  const char **exportFuncNameList = me.getExportFuncNameList();
  const char **exportForEachNameList = me.getExportForEachNameList();
  size_t i;

  for (i = 0; i < exportVarCount; ++i) {
    export_symbols.push_back(exportVarNameList[i]);
  }

  for (i = 0; i < exportFuncCount; ++i) {
    export_symbols.push_back(exportFuncNameList[i]);
  }

  // Expanded foreach functions should not be internalized, too.
  // expanded_foreach_funcs keeps the .expand version of the kernel names
  // around until createInternalizePass() is finished making its own
  // copy of the visible symbols.
  std::vector<std::string> expanded_foreach_funcs;
  for (i = 0; i < exportForEachCount; ++i) {
    expanded_foreach_funcs.push_back(
        std::string(exportForEachNameList[i]) + ".expand");
  }

  for (i = 0; i < exportForEachCount; i++) {
      export_symbols.push_back(expanded_foreach_funcs[i].c_str());
  }

  pPM.add(llvm::createInternalizePass(export_symbols));

  return true;
}

bool Compiler::addInvokeHelperPass(llvm::legacy::PassManager &pPM) {
  llvm::Triple arch(getTargetMachine().getTargetTriple());
  if (arch.isArch64Bit()) {
    pPM.add(createRSInvokeHelperPass());
  }
  return true;
}

bool Compiler::addExpandForEachPass(Script &pScript, llvm::legacy::PassManager &pPM) {
  // Expand ForEach on CPU path to reduce launch overhead.
  bool pEnableStepOpt = true;
  pPM.add(createRSForEachExpandPass(pEnableStepOpt));

  return true;
}

bool Compiler::addGlobalInfoPass(Script &pScript, llvm::legacy::PassManager &pPM) {
  // Add additional information about RS global variables inside the Module.
  RSScript &script = static_cast<RSScript &>(pScript);
  if (script.getEmbedGlobalInfo()) {
    pPM.add(createRSGlobalInfoPass(script.getEmbedGlobalInfoSkipConstant()));
  }

  return true;
}

bool Compiler::addInvariantPass(llvm::legacy::PassManager &pPM) {
  // Mark Loads from RsExpandKernelDriverInfo as "load.invariant".
  // Should run after ExpandForEach and before inlining.
  pPM.add(createRSInvariantPass());

  return true;
}

bool Compiler::addCustomPasses(Script &pScript, llvm::legacy::PassManager &pPM) {
  if (!addInvokeHelperPass(pPM))
    return false;

  if (!addExpandForEachPass(pScript, pPM))
    return false;

  if (!addInvariantPass(pPM))
    return false;

  if (!addInternalizeSymbolsPass(pScript, pPM))
    return false;

  if (!addGlobalInfoPass(pScript, pPM))
    return false;

  return true;
}

bool Compiler::addPostLTOCustomPasses(llvm::legacy::PassManager &pPM) {
  // Add pass to correct calling convention for X86-64.
  llvm::Triple arch(getTargetMachine().getTargetTriple());
  if (arch.getArch() == llvm::Triple::x86_64)
    pPM.add(createRSX86_64CallConvPass());

  // Add pass to mark script as threadable.
  pPM.add(createRSIsThreadablePass());

  return true;
}

enum Compiler::ErrorCode Compiler::screenGlobalFunctions(Script &pScript) {
  llvm::Module &module = pScript.getSource().getModule();

  // Materialize the bitcode module in case this is a lazy-load module.  Do not
  // clear the materializer by calling materializeAllPermanently since the
  // runtime library has not been merged into the module yet.
  if (module.getMaterializer() != nullptr) {
    std::error_code ec = module.materializeAll();
    if (ec) {
      ALOGE("Failed to materialize module `%s' when screening globals! (%s)",
            module.getModuleIdentifier().c_str(), ec.message().c_str());
      return kErrMaterialization;
    }
  }

  // Add pass to check for illegal function calls.
  llvm::legacy::PassManager pPM;
  pPM.add(createRSScreenFunctionsPass());
  pPM.run(module);

  return kSuccess;

}
