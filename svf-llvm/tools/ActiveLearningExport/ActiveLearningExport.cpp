#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVFIR/SVFVariables.h"
#include "WPA/Andersen.h"
#include "Graphs/SVFG.h"
#include "Graphs/SVFGNode.h"
#include "Graphs/SVFGEdge.h"
#include "Util/CommandLine.h"
#include "GraphReaderSVFGBuilder.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace SVF;

namespace
{

struct Args
{
    std::string outputDir;
    std::vector<std::string> modules;
};

struct SourceLocation
{
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
    std::string raw;
};

Args parseArgs(int argc, char** argv)
{
    Args args;
    std::vector<char*> forwarded;
    forwarded.push_back(argv[0]);
    for (int i = 1; i < argc; ++i)
    {
        const std::string item = argv[i];
        if ((item == "--output-dir" || item == "-o") && i + 1 < argc)
        {
            args.outputDir = argv[++i];
            continue;
        }
        forwarded.push_back(argv[i]);
    }
    forwarded.push_back(nullptr);
    args.modules = OptionBase::parseOptions(
        static_cast<int>(forwarded.size()) - 1,
        forwarded.data(),
        "SVFmemplus active-learning graph export",
        "[options] --output-dir <dir> <input-bitcode...>");
    return args;
}

int nodeType(const VFGNode* node)
{
    if (node == nullptr)
        return 0;
    switch (node->getNodeKind())
    {
    case SVFValue::Addr:
        return 1;
    case SVFValue::Load:
        return 2;
    case SVFValue::Store:
        return 3;
    case SVFValue::Copy:
        return 4;
    case SVFValue::Gep:
        return 5;
    case SVFValue::TPhi:
    case SVFValue::TIntraPhi:
    case SVFValue::TInterPhi:
    case SVFValue::MPhi:
    case SVFValue::MIntraPhi:
    case SVFValue::MInterPhi:
        return 6;
    case SVFValue::AParm:
    case SVFValue::FParm:
    case SVFValue::APIN:
    case SVFValue::APOUT:
    case SVFValue::FPIN:
    case SVFValue::FPOUT:
        return 9;
    case SVFValue::Branch:
        return 11;
    default:
        return 19;
    }
}

int edgeType(const VFGEdge* edge)
{
    if (edge == nullptr)
        return 0;
    switch (edge->getEdgeKind())
    {
    case VFGEdge::IntraDirectVF:
        return 0;
    case VFGEdge::IntraIndirectVF:
        return 1;
    case VFGEdge::CallDirVF:
        return 2;
    case VFGEdge::RetDirVF:
        return 3;
    case VFGEdge::CallIndVF:
        return 4;
    case VFGEdge::RetIndVF:
        return 5;
    case VFGEdge::TheadMHPIndirectVF:
        return 6;
    default:
        return 0;
    }
}

int patternForEdgeType(int type)
{
    if (type == 0 || type == 2 || type == 3)
        return 1;
    if (type == 1)
        return 2;
    if (type == 4 || type == 5 || type == 6)
        return 4;
    return 1;
}

bool nodeTouchesObject(const VFGNode* node, NodeID objectId)
{
    if (node == nullptr)
        return false;
    NodeBS vars = node->getDefSVFVars();
    return vars.test(objectId);
}

std::string csvEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value)
    {
        if (ch == '"')
            escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

SourceLocation sourceLocationForObject(const BaseObjVar* object)
{
    SourceLocation loc;
    if (object == nullptr || object->getICFGNode() == nullptr)
        return loc;

    loc.raw = object->getICFGNode()->getSourceLoc();
    std::smatch match;
    if (std::regex_search(loc.raw, match, std::regex("\"fl\"\\s*:\\s*\"([^\"]+)\"")))
        loc.file = match[1].str();
    if (std::regex_search(loc.raw, match, std::regex("\"ln\"\\s*:\\s*([0-9]+)")))
        loc.line = static_cast<unsigned>(std::stoul(match[1].str()));
    if (std::regex_search(loc.raw, match, std::regex("\"cl\"\\s*:\\s*([0-9]+)")))
        loc.column = static_cast<unsigned>(std::stoul(match[1].str()));
    return loc;
}

std::vector<const VFGNode*> anchorsForObject(const SVFG* svfg, NodeID objectId)
{
    std::vector<const VFGNode*> anchors;
    for (auto it = svfg->begin(), eit = svfg->end(); it != eit; ++it)
    {
        const VFGNode* node = it->second;
        if (nodeTouchesObject(node, objectId))
            anchors.push_back(node);
    }
    return anchors;
}

std::set<const VFGNode*> collectNeighborhood(
    const std::vector<const VFGNode*>& anchors,
    unsigned depth)
{
    std::set<const VFGNode*> selected;
    std::queue<std::pair<const VFGNode*, unsigned>> work;
    for (const VFGNode* anchor : anchors)
    {
        if (selected.insert(anchor).second)
            work.push({anchor, 0});
    }
    while (!work.empty())
    {
        const VFGNode* node = work.front().first;
        const unsigned level = work.front().second;
        work.pop();
        if (level >= depth)
            continue;
        for (auto edgeIt = node->OutEdgeBegin(), edgeEit = node->OutEdgeEnd(); edgeIt != edgeEit; ++edgeIt)
        {
            const VFGNode* next = (*edgeIt)->getDstNode();
            if (selected.insert(next).second)
                work.push({next, level + 1});
        }
        for (auto edgeIt = node->InEdgeBegin(), edgeEit = node->InEdgeEnd(); edgeIt != edgeEit; ++edgeIt)
        {
            const VFGNode* next = (*edgeIt)->getSrcNode();
            if (selected.insert(next).second)
                work.push({next, level + 1});
        }
    }
    return selected;
}

void writeGraph(
    const std::filesystem::path& outputDir,
    unsigned graphIndex,
    const BaseObjVar* object,
    const SVFG* svfg)
{
    const std::vector<const VFGNode*> anchors = anchorsForObject(svfg, object->getId());
    std::set<const VFGNode*> selected = collectNeighborhood(anchors, 2);

    std::map<const VFGNode*, unsigned> localIds;
    unsigned nextId = 1;
    for (const VFGNode* node : selected)
        localIds[node] = nextId++;

    std::map<const VFGNode*, int> pattern;
    std::map<const VFGNode*, int> pointedBy;
    struct LocalEdge
    {
        unsigned src;
        unsigned dst;
        int type;
    };
    std::vector<LocalEdge> edges;
    for (const auto& item : localIds)
    {
        const VFGNode* node = item.first;
        for (auto edgeIt = node->OutEdgeBegin(), edgeEit = node->OutEdgeEnd(); edgeIt != edgeEit; ++edgeIt)
        {
            const VFGEdge* edge = *edgeIt;
            auto dst = localIds.find(edge->getDstNode());
            if (dst == localIds.end())
                continue;
            const int type = edgeType(edge);
            edges.push_back({item.second, dst->second, type});
            const int bit = patternForEdgeType(type);
            pattern[node] |= bit;
            pattern[edge->getDstNode()] |= bit;
            pointedBy[edge->getDstNode()] += 1;
        }
    }

    if (selected.empty())
    {
        // A modeled heap object may have no SVFG node after optimization. Emit a
        // single valid anchor graph so downstream inference still sees the object.
        std::ofstream nodeOut(outputDir / (std::to_string(graphIndex) + ".node.csv"));
        nodeOut << "id,pattern,type,level,pointedBy\n";
        nodeOut << "1,7,12,0,0\n";
        std::ofstream edgeOut(outputDir / (std::to_string(graphIndex) + ".edge.csv"));
        edgeOut << "srcid,tgtid,type\n";
        return;
    }

    std::ofstream nodeOut(outputDir / (std::to_string(graphIndex) + ".node.csv"));
    nodeOut << "id,pattern,type,level,pointedBy\n";
    for (const auto& item : localIds)
    {
        int bits = pattern[item.first];
        if (bits == 0)
            bits = 7;
        nodeOut << item.second << "," << bits << "," << nodeType(item.first)
                << ",0," << pointedBy[item.first] << "\n";
    }

    std::ofstream edgeOut(outputDir / (std::to_string(graphIndex) + ".edge.csv"));
    edgeOut << "srcid,tgtid,type\n";
    for (const LocalEdge& edge : edges)
        edgeOut << edge.src << "," << edge.dst << "," << edge.type << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    Args args = parseArgs(argc, argv);
    if (args.outputDir.empty() || args.modules.empty())
    {
        SVFUtil::errs() << "usage: svf-al-export --output-dir <dir> <input.bc>\n";
        return 2;
    }

    LLVMModuleSet::buildSVFModule(args.modules);
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    GraphReaderSVFGBuilder svfgBuilder(true, false);
    SVFG* svfg = svfgBuilder.buildFullSVFG(ander);

    const std::filesystem::path out(args.outputDir);
    std::filesystem::create_directories(out);

    std::ofstream idOut(out / "idToGraph.csv");
    std::ofstream indexOut(out / "graph_index.csv");
    indexOut << "graph_id,object_id,file,line,column,source_loc\n";
    unsigned graphIndex = 0;
    for (auto it = pag->begin(), eit = pag->end(); it != eit; ++it)
    {
        const SVFVar* var = it->second;
        const BaseObjVar* object = SVFUtil::dyn_cast<BaseObjVar>(var);
        if (object == nullptr || !object->isHeap())
            continue;
        const std::string graphId = "heap:" + std::to_string(object->getId());
        const SourceLocation loc = sourceLocationForObject(object);
        idOut << graphId << "\n";
        indexOut << csvEscape(graphId) << "," << object->getId() << ","
                 << csvEscape(loc.file) << "," << loc.line << "," << loc.column << ","
                 << csvEscape(loc.raw) << "\n";
        writeGraph(out, graphIndex, object, svfg);
        ++graphIndex;
    }

    if (graphIndex == 0)
    {
        idOut << "heap:none\n";
        indexOut << csvEscape("heap:none") << ",0,\"\",0,0,\"\"\n";
        std::ofstream nodeOut(out / "0.node.csv");
        nodeOut << "id,pattern,type,level,pointedBy\n1,7,12,0,0\n";
        std::ofstream edgeOut(out / "0.edge.csv");
        edgeOut << "srcid,tgtid,type\n";
    }

    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
