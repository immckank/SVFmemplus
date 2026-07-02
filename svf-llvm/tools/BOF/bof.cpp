//===- bof.cpp -- Buffer Overflow Errors Checker -------------------------------------//
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
 // Buffer Overflow Errors Checker 
 //
 // Author: Yaokun Yang,
 */

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "BOF/BufferOverflowChecker.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"


using namespace llvm;
using namespace SVF;

static const Option<std::string> BofReportFile(
    "bof-report", "Dump the buffer-overflow bug report to the given JSON file", "");

// ---- "Client-special edition" display switch (output only) ----
// When set, MAY (possible) overflows are rendered as MUST in the terminal
// output and the JSON report. Detection / classification is unchanged; the
// default (off) is what the experiments use.
static const Option<bool> BofMayAsMust(
    "bof-may-as-must",
    "Render MAY (possible) overflows as MUST in the output (display only)",
    false);

// ---- LLM MAY-triage overlay (pure add-on; never alters sound results) ----
static const Option<std::string> LlmConfigFile(
    "llm-config",
    "JSON config for the LLM MAY-triage sidecar (api_url/api_key/model/threshold)",
    "");
static const Option<std::string> LlmSliceOut(
    "llm-slice-out",
    "Path for the exported bof-slice/v1 JSON (always written when MAYs survive)",
    "bof_slices.json");
static const Option<std::string> LlmSidecar(
    "llm-sidecar",
    "Path to the Python triage sidecar (llm_triage.py); empty => API-empty mode",
    "");

int main(int argc, char** argv)
{
    auto moduleNameVec =
        OptionBase::parseOptions(argc, argv, "Buffer Overflow Errors Checker",
                                 "[options] <input-bitcode...>");

    // Refers to content of a singleton unique_ptr<SVFIR> in SVFIR.
    SVFIR* pag = nullptr;

    if (Options::ReadJson())
    {
        assert(false && "please implement SVFIRReader::read");
    }
    else
    {
        if (Options::WriteAnder() == "ir_annotator")
        {
            LLVMModuleSet::preProcessBCs(moduleNameVec);
        }

        LLVMModuleSet::buildSVFModule(moduleNameVec);

        /// Build SVFIR
        SVFIRBuilder builder;
        pag = builder.build();

    }

    BufferOverflowChecker bufferOverflowChecker;

    // Assemble the LLM MAY-triage config: optional JSON file first, then env
    // fallback (BOF_LLM_API_URL/KEY/MODEL), then CLI overrides for paths.
    LLMTriageConfig llmCfg;
    if (!LlmConfigFile().empty())
        llmCfg.loadFromFile(LlmConfigFile());
    llmCfg.loadFromEnv();
    if (!LlmSliceOut().empty())
        llmCfg.sliceOutPath = LlmSliceOut();
    if (!LlmSidecar().empty())
        llmCfg.sidecarPath = LlmSidecar();
    bufferOverflowChecker.setLLMTriageConfig(llmCfg);

    // Client-special edition: display MAY as MUST (output only).
    bufferOverflowChecker.setMayAsMust(BofMayAsMust());

    bufferOverflowChecker.runOnModule(pag);

    // Optionally persist the structured bug report as JSON.
    if (!BofReportFile().empty())
        bufferOverflowChecker.dumpReport(BofReportFile());

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
