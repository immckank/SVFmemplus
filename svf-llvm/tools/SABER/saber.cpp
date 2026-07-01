//===- saber.cpp -- Source-sink bug checker------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2017>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===-----------------------------------------------------------------------===//

/*
 // Saber: Software Bug Check.
 //
 // Author: Yulei Sui,
 */

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SABER/LeakChecker.h"
#include "SABER/FileChecker.h"
#include "SABER/DoubleFreeChecker.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "Util/Z3Expr.h"

#include "SABER/UseAfterFreeChecker.h"
#include "SABER/UninitChecker.h"
#include "SABER/SaberSemanticRules.h"
#include <filesystem>


using namespace llvm;
using namespace SVF;

static const Option<std::string> ReportDir(
    "report-dir",
    "Directory containing the per-alert Saber JSON tree",
    ".");
static const Option<std::string> SaberSemanticRulesPath(
    "saber-semantic-rules", "Load approved semantic-rules/v1 JSON", "");

static void setDefaultReportConfig(const std::vector<std::string>& modules)
{
    const std::filesystem::path dir =
        ReportDir().empty() ? std::filesystem::path(".") : std::filesystem::path(ReportDir());
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error)
    {
        SVFUtil::errs() << "[SaberReport] cannot create report directory "
                        << dir.string() << ": " << error.message() << "\n";
        std::exit(EXIT_FAILURE);
    }
    LeakChecker::setAlertOutputDir((dir / "alerts").string());
}

int main(int argc, char ** argv)
{

    std::vector<std::string> moduleNameVec;
    moduleNameVec = OptionBase::parseOptions(
                        argc, argv, "Source-Sink Bug Detector", "[options] <input-bitcode...>"
                    );

    if (Options::WriteAnder() == "ir_annotator")
    {
        LLVMModuleSet::preProcessBCs(moduleNameVec);
    }

    LLVMModuleSet::buildSVFModule(moduleNameVec);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    if (!SaberSemanticRulesPath().empty() &&
            !SaberSemanticRules::get()->loadFile(SaberSemanticRulesPath()))
        SVFUtil::errs() << "[SaberSemanticRules] rules rejected; using built-in semantics only\n";


    std::unique_ptr<LeakChecker> saber;

    if(Options::MemoryLeakCheck())
        saber = std::make_unique<LeakChecker>();
    else if(Options::FileCheck())
        saber = std::make_unique<FileChecker>();
    else if(Options::DFreeCheck())
        saber = std::make_unique<DoubleFreeChecker>();
    else if(Options::UAFCheck())
        saber = std::make_unique<UseAfterFreeChecker>();
    else if(Options::UninitCheck())
        saber = std::make_unique<UninitChecker>();
    else
        saber = std::make_unique<LeakChecker>();  // if no checker is specified, we use leak checker as the default one.

    setDefaultReportConfig(moduleNameVec);

    saber->runOnModule(pag);
    saber.reset();
    LLVMModuleSet::releaseLLVMModuleSet();

    return 0;

}
