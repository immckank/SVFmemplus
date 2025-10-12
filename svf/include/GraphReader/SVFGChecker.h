// #ifndef SVFGCHECKER_H_
// #define SVFGCHECKER_H_

// #include "SABER/SrcSnkDDA.h"
// #include "Graphs/SVFGOPT.h"
// #include "SABER/ProgSlice.h"
// #include "SABER/SaberSVFGBuilder.h"
// #include "Util/GraphReachSolver.h"
// #include "Util/SVFBugReport.h"

// namespace SVF {

// class SVFGChecker : public SrcSnkDDA {

// public:
//     struct LocationAndVar {
//         std::string location; // "filename.c:line"
//         std::string varName;
//     };

//     SVFGChecker() {
//         // 在这里可以设置你关心的源和汇
//         // 示例：
//         sourceLocs.push_back({"items.c:1557", "cdata"});
//         sinkLocs.push_back({"items.c:1630", "cdata"});
//     }

//     virtual const std::string getName() const {
//         return "SVFGChecker";
//     }

// protected:
//     /// 重载源和汇的初始化方法
//     virtual void initSrcs() override;
//     virtual void initSnks() override;

//     /// 重载bug报告方法以打印路径条件
//     virtual void reportBug(ProgSlice* slice) override;

// private:
//     /// 辅助函数：根据代码位置和变量名查找SVFG节点
//     SVFGNodeSet findSVFGNodesByLocationAndVar(const std::vector<LocationAndVar>& locs);

//     /// 辅助函数：从字符串中解析文件名和行号
//     //bool parseLocation(const std::string& location, std::string& filename, unsigned& line);

//     /// 存储用户指定的源和汇的位置/变量信息
//     std::vector<LocationAndVar> sourceLocs;
//     std::vector<LocationAndVar> sinkLocs;
// };

// } // namespace SVF

// #endif /* CUSTOMCHECKER_H_ */