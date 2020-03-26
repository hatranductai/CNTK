//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "Basics.h"
#include "File.h"
#include "Matrix.h"
#include "Config.h"

#include "ComputationNode.h"
#include "ScriptableObjects.h"
#include "ComputationEnvironment.h"
#include "RecurrentNodes.h"

#include <map>
#include <string>
#include <stdexcept>
#include <list>
#include <vector>
#include <deque>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <iostream>
#include <regex>
#include <chrono>
#include <unordered_map>
#include <set>

#include "ComputationGraphAlgorithms.h"
#include "DataReader.h"
#include "ReshapingNodes.h"
// #include "..\SGDlib\SimpleOutputWriter.h"

namespace Microsoft
{
namespace MSR
{
namespace CNTK
{
/*
struct PathInfo
{
    float WER;
    float prob;
    vector<size_t> label_seq;
};
*/

    

inline std::wstring ToString(const ComputationNodeBasePtr& node)
{
    return node->NodeName();
}

// ===========================================================================
// ComputationNetwork -- computation graph and operations
// ===========================================================================
// move the shared function/data structure from SimpleOutputWriter.h to this file

class ComputationNetwork : public ScriptableObjects::Object,
                           public ScriptableObjects::HasToString,
                           public ScriptableObjects::CustomConfigRecord
{
public:
    // we have to give template <class ElemType> for each function and struct, rather than put in on top of class ComputationNetwork, because if we do so, we have to pass the ElemType for each occurence that uses computationnetwork

    typedef shared_ptr<ComputationNetwork> ComputationNetworkPtr;

    // -----------------------------------------------------------------------
    // construction
    // -----------------------------------------------------------------------

    ComputationNetwork()
        : m_randomSeedOffset(0),
          m_isCompiled(false),
          m_areMatricesAllocated(false),
          m_pMBLayoutOfNetwork(make_shared<MBLayout>(1, 0, ComputationNodeBase::DefaultDynamicAxisName)),
          m_environment(make_shared<ComputationEnvironment>())
    {
        //m_pMBLayoutOfNetwork->SetAxisName(L"T");
    }

    ComputationNetwork(DEVICEID_TYPE deviceId)
        : ComputationNetwork()
    {
        SetDeviceId(deviceId);
    }
    ComputationNetwork(const ScriptableObjects::IConfigRecordPtr configp); // construct from config

    virtual ~ComputationNetwork()
    {
        ClearNetwork(); // This will explicitly remove all nodes. This is needed to break circular references in loops.
    }

    void ClearNetwork();
    void InvalidateCompiledNetwork();

    void SetDeviceId(DEVICEID_TYPE deviceId)
    {
        m_deviceId = deviceId;
    }

    DEVICEID_TYPE GetDeviceId() const
    {
        return m_deviceId;
    }

    void PrintNodeTiming();

protected:
    void ConstructFromRoots(DEVICEID_TYPE deviceId, std::deque<ComputationNodeBasePtr>&& roots, const map<ComputationNodeBasePtr, ComputationNodeBasePtr>& replacements);
    void ProcessSpecialNodes(const ScriptableObjects::IConfigRecord& config, std::deque<ComputationNodeBasePtr>& roots);

public:
    // -----------------------------------------------------------------------
    // (de-)serialization
    // -----------------------------------------------------------------------
    template <class ElemType>
    void ReadPersistableParameters(size_t modelVersion, File& fstream, bool create);
    // reload node content only, e.g. used by SGD::Train() when going back to an older model that had better training objective
    template <class ElemType>
    void RereadPersistableParameters(const std::wstring& fileName)
    {
        File fstream(fileName, FileOptions::fileOptionsBinary | FileOptions::fileOptionsRead);
        auto modelVersion = GetModelVersion(fstream);
        ReadPersistableParameters<ElemType>(modelVersion, fstream, false);
    }
    // design BUGBUG: binary files do not know whether they are float or double.
    // TODO: modify file format to know this; then eliminate the <ElemType> dependency (and in some future, allow nodes to be different)
    template <class ElemType>
    void Read(const std::wstring& fileName);
    template <class ElemType>
    void Load(const std::wstring& fileName)
    {
        Read<ElemType>(fileName);
        // perform all further post-processing, caching, etc.
        CompileNetwork();
    }

    // static helper to instantiate a network from a file
    template <class ElemType>
    static ComputationNetworkPtr CreateFromFile(DEVICEID_TYPE deviceId, const std::wstring& fileName)
    {
        auto net = make_shared<ComputationNetwork>(deviceId);
        net->Load<ElemType>(fileName);
        return net;
    }

    void Save(const std::wstring& fileName, const FileOptions fileFormat = FileOptions::fileOptionsBinary) const;
    void SaveEdited(const std::wstring& fileName, const FileOptions fileFormat = FileOptions::fileOptionsBinary);

private:
    void SaveToFileImpl(const std::wstring& fileName, const FileOptions fileFormat) const;

    static size_t GetModelVersion(File& fstream);

public:
    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // main entry point for forward prop
    void ForwardProp(const ComputationNodeBasePtr rootNode);

    // main entry point for post forward and backward prop
    void PostForwardAndBackProp(const ComputationNodeBasePtr rootNode);

    // main entry point for backprop
    void Backprop(const ComputationNodeBasePtr rootNode);

    template <class NODESET> // version that takes multiple nodes
    void TravserseInSortedGlobalEvalOrder(const NODESET& nodes, const std::function<void(const ComputationNodeBasePtr&)>& action)
    {
        // Create a composite evaluation order for all the nodes
        std::vector<ComputationNodeBasePtr> combinedEvalOrder;
        for (auto node : nodes)
        {
            auto currentNodeEvalOrder = GetEvalOrder(node);
            combinedEvalOrder.insert(combinedEvalOrder.end(), currentNodeEvalOrder.begin(), currentNodeEvalOrder.end());
        }

        combinedEvalOrder = SortByGlobalEvalOrder(combinedEvalOrder);
        set<ComputationNodeBasePtr> completedSEQNodes;
        for (auto& node : combinedEvalOrder)
        {
            if (node->IsPartOfLoop())
            {
                shared_ptr<SEQTraversalFlowControlNode> recInfo = FindInRecurrentLoops(m_allSEQNodes, node);
                assert(recInfo != nullptr);
                if (completedSEQNodes.insert(recInfo).second)
                    node = recInfo;
                else
                    node = nullptr;
            }

            if (node)
                action(node);
        }
    }

    template <class NODESET> // version that takes multiple nodes
    void ForwardProp(const NODESET& nodes)
    {
        TravserseInSortedGlobalEvalOrder(nodes, [](const ComputationNodeBasePtr& node) {
            PARTraversalFlowControlNode::ForwardProp(node, FrameRange(nullptr));
        });
    }

    template <class NODESET> // version that takes multiple nodes
    void PostForwardAndBackProp(const NODESET& nodes)
    {
        TravserseInSortedGlobalEvalOrder(nodes, [](const ComputationNodeBasePtr& node) {
            PARTraversalFlowControlNode::PostForwardAndBackProp(node);
        });
    }

    template <class NODESET_FROM, class NODESET_TO> // version that takes both initial and final set of nodes
    void ForwardPropFromTo(const NODESET_FROM& nodesFrom, const NODESET_TO& nodesTo)
    {
        // Compute the set of nodes to do forward on.
        std::set<ComputationNodeBasePtr> nodesToForward;
        TravserseInSortedGlobalEvalOrder(nodesTo, [&](const ComputationNodeBasePtr& node) {
            for (const ComputationNodeBasePtr& input : node->GetInputs())
            {
                if (std::find(nodesFrom.begin(), nodesFrom.end(), input) != nodesFrom.end() || nodesToForward.find(input) != nodesToForward.end())
                {
                    nodesToForward.insert(node);
                }
            }
        });

        // Perform forward on resulting nodes in global evaluation order.
        for (const auto& node : SortByGlobalEvalOrder(nodesToForward))
        {
            ComputationNetwork::PARTraversalFlowControlNode::ForwardProp(node, FrameRange(nullptr));
        }
    }

    static void BumpEvalTimeStamp(const std::vector<ComputationNodeBasePtr>& nodes);
    void ResetEvalTimeStamps();
    void SetEvalTimeStampsOutdatedWithRegardToAll();

    // and for a set of nodes
    void StartEvaluateMinibatchLoop(const ComputationNodeBasePtr& rootNode) // (ugly name; meant to be unique so we can rename if needed)
    {
        VerifyIsCompiled("StartEvaluateMinibatchLoop");
        ResetEvalTimeStamps(); // invalidate all m_value fields  --TODO: redundant (called over again for every root node). Make this private and only call for sets of nodes.
        for (auto& node : GetEvalOrder(rootNode))
            node->OnEpochStart();
    }
    template <class NODESET>
    void StartEvaluateMinibatchLoop(const NODESET& nodes) // (ugly name; meant to be unique so we can rename if needed)
    {
        for (auto& node : nodes)
            StartEvaluateMinibatchLoop(node);
    }
    template <class NODESET>
    void StartEvaluateMinibatchLoop(const NODESET& nodes1, const NODESET& nodes2) // often needed for two sets (training & evaluation criteria)
    {
        StartEvaluateMinibatchLoop(nodes1);
        StartEvaluateMinibatchLoop(nodes2);
    }

    // -----------------------------------------------------------------------
    // evaluation: preparation
    // -----------------------------------------------------------------------

    void CompileNetwork(); // call this after creation, Load(), and any modification
    void ValidateNetwork();

private:
    size_t ValidateNodes(list<ComputationNodeBasePtr> nodes, bool isFirstPass, bool isFinalValidationPass);
    bool ValidateNode(ComputationNodeBasePtr node, bool isFinalValidationPass) const;
    void MarkValueNonSharableNodes();
    void ChangeNodeInputs(ComputationNodeBasePtr fromNode, ComputationNodeBasePtr toNode);

private:
    void DetermineSetOfAllRoots();
    void CollectInputAndLearnableParameters(const ComputationNodeBasePtr& rootNode);
    void CollectInputAndLearnableParametersRec(const ComputationNodeBasePtr& node, set<ComputationNodeBasePtr>& visited, list<ComputationNodeBasePtr>& inputs, list<ComputationNodeBasePtr>& learnableParameters);
    void ResetMBLayouts();
    bool IsCompiled() const
    {
        return m_isCompiled;
    }
    bool AreMatricesAllocated() const
    {
        return m_areMatricesAllocated;
    }
    void VerifyIsCompiled(const char* where) const;

public:
    void AllocateAllMatrices(const std::vector<ComputationNodeBasePtr>& evalRootNodes, const std::vector<ComputationNodeBasePtr>& outValueRootNodes, ComputationNodeBasePtr trainRootNode);

    // From the set of nodes extract all nodes which are used as accumulator nodes.
    std::set<ComputationNodeBasePtr> ExtractNodesWhichAccumulateResult(std::set<ComputationNodeBasePtr> nodes);

private:
    void PrintMemorySharingStructure(const std::vector<ComputationNodeBasePtr>& nodes);
    void ReleaseMatricesAfterEvalForChildren(ComputationNodeBasePtr n, std::unordered_map<ComputationNodeBasePtr, std::unordered_set<ComputationNodeBasePtr>>& parentsMap);

public:
    // -----------------------------------------------------------------------
    // evaluation: execution plan and network recurrent-loop analysis
    // -----------------------------------------------------------------------

    void FormNestedNetwork(const ComputationNodeBasePtr& rootNode);
    ComputationNodeBasePtr GetNestedNetwork(const ComputationNodeBasePtr& rootNode);

private:
    // The method below determines evaluation order, which is tricky in presence of recurrent loops.
    void FormRecurrentLoops();

public:
    // -----------------------------------------------------------------------
    // evaluation: traversal
    // These three functions create and cache traversal orders of the network.
    // -----------------------------------------------------------------------

    // determine the required order in which nodes must be computed in order to compute 'rootNode'
    // If passed nullptr, this will traverse the entire net.
    // If passed non-null, it will take the global traveral in ITS order and sub-filter against root's dependents.
    void FormEvalOrder(const ComputationNodeBasePtr& rootNode)
    {
        if (m_evalOrders.find(rootNode) != m_evalOrders.end())
        {
            if (rootNode)
                fprintf(stderr, "FormEvalOrder: WARNING: Was called twice for %ls %ls operation.\n", rootNode->NodeName().c_str(), rootNode->OperationName().c_str());
            else
                fprintf(stderr, "FormEvalOrder: WARNING: Was called twice.\n");
        }

        std::list<ComputationNodeBasePtr> evalOrder;
        ExecutionGraph graph(m_allRoots);
        if (!rootNode) // this creates the global list of nodes
        {
            evalOrder = ::CNTK::PostOrderTraversal(graph, m_allRoots);
        }
        else // this creates a subset of the global eval order of all nodes that rootNode depends on
        {
            auto rawTraversalForRoot = ::CNTK::PostOrderTraversal(graph, {rootNode}); // traverse to find the set (we ignore the order)
            set<ComputationNodeBasePtr> rawSet(rawTraversalForRoot.begin(), rawTraversalForRoot.end());
            for (const auto& node : GetEvalOrder(nullptr)) // iterate over global one and pull out everything that is included in the set for rootNode
            {
                if (rawSet.find(node) != rawSet.end())
                    evalOrder.push_back(node);
            }
        }
        m_evalOrders[rootNode] = evalOrder;
    }

    template <typename ContainerType>
    std::vector<ComputationNodeBasePtr> SortByGlobalEvalOrder(const ContainerType& nodesToSort)
    {
        std::vector<ComputationNodeBasePtr> sortedEvalOrder;
        if (nodesToSort.size() == 1)
            sortedEvalOrder.assign(nodesToSort.cbegin(), nodesToSort.cend());
        else
        {
            const std::list<ComputationNodeBasePtr>& allNodesEvalOrder = GetEvalOrder(nullptr);
            for (auto& node : allNodesEvalOrder)
            {
                if (std::find(nodesToSort.cbegin(), nodesToSort.cend(), node) != nodesToSort.cend())
                    sortedEvalOrder.push_back(node);
            }
        }

        return sortedEvalOrder;
    }

    // replace an existing eval order with an updated one
    // This is meant to be used by FormRecurrentLoops().  TODO: Hopefully this can be not done anymore some day.
    void UpdateEvalOrder(const ComputationNodeBasePtr& rootNode, const std::list<ComputationNodeBasePtr>& nodes)
    {
        GetEvalOrder(rootNode); // verify that there is already an entry for rootNode
        m_evalOrders[rootNode] = nodes;
    }

    bool EvalOrderExists(const ComputationNodeBasePtr& rootNode) const
    {
        return m_evalOrders.find(rootNode) != m_evalOrders.end();
    }

    // get depth-first traversal order
    // TODO: This is currently not immutable because it gets patched w.r.t. recurrent loops. Ideally we don't patch. Need to review and verify that it is sufficient.
    const std::list<ComputationNodeBasePtr>& GetEvalOrder(const ComputationNodeBasePtr& rootNode) const
    {
        auto iter = m_evalOrders.find(rootNode);
        if (iter == m_evalOrders.end())
        {
            LogicError("GetEvalOrder: Called without prior call to FormEvalOrder() for %ls %ls operation", rootNode->NodeName().c_str(), rootNode->OperationName().c_str());
        }
        return iter->second;
    }

    // same as GetEvalOrder() where ordering is irrelevant
    const std::list<ComputationNodeBasePtr>& GetAllNodesForRoot(const ComputationNodeBasePtr& rootNode) const
    {
        return GetEvalOrder(rootNode);
    }

protected:
    class SEQTraversalFlowControlNode;

private:
    static std::shared_ptr<SEQTraversalFlowControlNode> FindInRecurrentLoops(const std::vector<std::shared_ptr<SEQTraversalFlowControlNode>>& recurrentInfo, const ComputationNodeBasePtr& node);

public:
    // -----------------------------------------------------------------------
    // MBLayouts
    // -----------------------------------------------------------------------

    // Note: this is also used to copy MBLayouts into our existing MBLayout instance, which is a somewhat questionable design.
    // BUGBUG (Issue #95): This function will conflict once we have multiple input layouts in the network.
    const MBLayoutPtr& GetMBLayoutPtrOfNetwork()
    {
        return m_pMBLayoutOfNetwork;
    }

    // determine the actual MB size from the feature nodes
    // This returns max number of columns over the feature nodes.
    // Note that if we have multiple slices, MB size != #frames.
    // BUGBUG: This will break once we have inconsistent layouts.
    // BUGBUG: The number computed here is completely off (it the layout has gaps
    // they will also be counted towards the actualMBSize)
    size_t DetermineActualMBSizeFromFeatures() const
    {
        size_t actualMBSize = 0;

        const auto& featureNodes = FeatureNodes(); // TODO: a getter; should be called GetFeatureNodes()
        for (auto& nodeIter : featureNodes)
            actualMBSize = max(actualMBSize, nodeIter->GetMBLayout()->GetNumCols());

        return actualMBSize;
    }

    // When external code (readers, namely) updates InputValue's m_value,
    // calling this function is required to make sure that any internal state gets updated correctly.
    // Only a change to the column dimension i sallowed
    void NotifyInputNodesFunctionValuesMBSizeModified()
    {
        for (auto& nodeIter : FeatureNodes())
            nodeIter->NotifyFunctionValuesMBSizeModified();
        for (auto& nodeIter : LabelNodes())
            nodeIter->NotifyFunctionValuesMBSizeModified();
    }

    // this counts the actual number of frames in a minibatch (not counting gaps in parallel sequences)
    // TODO: Instead of passing numAllSamples in here, we should determine it from the inputs in case of no layout. Or simply forbid this case.
    // BUGBUG (Issue #95): With variable-length sequences, this can no longer be a network method.
    size_t GetNumSamplesWithLabelOfNetwork(const size_t numAllSamples) const
    {
        if (m_pMBLayoutOfNetwork)
            return m_pMBLayoutOfNetwork->GetActualNumSamples();
        else
            return numAllSamples; // TODO: Return the actual number of samples, by inquiring our own input nodes; then eliminate the numAllSamples parameter.
    }

    // -----------------------------------------------------------------------
    // node construction
    // -----------------------------------------------------------------------

    // this function is only for use by NDL (deprecated)
    void InitLearnableParameters(const ComputationNodeBasePtr& node,
                                 const wchar_t* initString, // "uniform"|"gaussian"|"fixedValue"
                                 double initValue,          //  scale   | scale    | value
                                 unsigned long randomSeed = 0,
                                 bool initOnCPUOnly = false) const;
    // non-static version needed because it accesses m_randomSeedOffset
    // Legacy version that is for random only.
    void RandomInitLearnableParameters(const ComputationNodeBasePtr& node, const bool uniformInit, const unsigned long randomSeed, const double initValueScale, bool initOnCPUOnly = false) const;

    template <class ElemType>
    void InitLearnableParametersWithBilinearFill(const ComputationNodeBasePtr& node, size_t kernelWidth, size_t kernelHeight);

    template <typename N>
    static shared_ptr<N> AsNodePtr(const ComputationNodeBasePtr& inode)
    {
        return dynamic_pointer_cast<N>(inode);
    }
    template <typename N>
    static bool IsNodePtr(const ComputationNodeBasePtr& inode)
    {
        return AsNodePtr<N>(inode) != nullptr;
    }

    // -----------------------------------------------------------------------
    // network editing
    // -----------------------------------------------------------------------

    ComputationNodeBasePtr CopyNode(const ComputationNetwork& fromNet, const std::wstring fromName, std::wstring toName, const CopyNodeFlags flags);
    void CopySubTree(const ComputationNetwork& fromNet, const std::wstring fromName, std::wstring toNamePrefix, const CopyNodeFlags flags);
    void ShowNodeMemory(const ComputationNetwork& fromNet, const std::wstring fromName);
    void CopyInputs(const std::wstring fromName, std::wstring toName);
    void RenameNode(const std::wstring& nodeNameOrig, const std::wstring& nodeNameNew);
    void RenameNode(ComputationNodeBasePtr node, const std::wstring& newNodeName);
    void DeleteNode(const std::wstring& nodeName);
    void ReplaceNode(wstring nodeName, ComputationNodeBasePtr newNode);
    void InsertNode(wstring nodeName, ComputationNodeBasePtr newNode, const std::set<std::wstring>& newNodeTags);
    void ReplaceLeafNode(wstring oldNodeName, ComputationNodeBasePtr newNode);
    void ReplaceFinalCriterionNode(wstring oldNodeName, ComputationNodeBasePtr newNode);
    void AddFeatureNode(ComputationNodeBasePtr featureNode);
    //ComputationNodeBasePtr RemoveFeatureNode(ComputationNodeBasePtr featureNode);
    void SetLearnableNodesBelowLearningRateMultiplier(const float learningRateMultiplier, const ComputationNodeBasePtr& rootNode = nullptr);

    // -----------------------------------------------------------------------
    // node access
    // -----------------------------------------------------------------------

    bool NodeNameExists(const std::wstring& name) const
    {
        auto iter = m_nameToNodeMap.find(name);
        return (iter != m_nameToNodeMap.end());
    }

    ComputationNodeBasePtr GetNodeFromName(const std::wstring& name) const
    {
        auto iter = m_nameToNodeMap.find(name);
        if (iter == m_nameToNodeMap.end())
            RuntimeError("GetNodeFromName: Network has no node named '%ls'.", name.c_str());
        return iter->second;
    }

    // GetNodesFromName - Get all the nodes from a name that may match a wildcard '*' pattern
    //   only patterns with a single '*' at the beginning, in the middle, or at the end are accepted
    // name - node name (with possible wildcard)
    // returns: vector of nodes that match the pattern, may return an empty vector for no match
    std::vector<ComputationNodeBasePtr> GetNodesFromName(const std::wstring& name) const
    {
        std::vector<ComputationNodeBasePtr> nodes;
        size_t found = name.find_first_of(L'*');
        if (found == std::wstring::npos)
        {
            if (NodeNameExists(name))
                nodes.push_back(GetNodeFromName(name));
        }
        else
        {
            std::wstring head = name.substr(0, found);
            std::wstring tail = name.substr(found + 1);
            for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
            {
                const wstring& nodeName = nodeIter->first;

                // if it matches on both ends (we only support A*B patterns it's a match
                bool headMatch = head.empty() || nodeName.find(head) == 0;
                bool tailMatch = tail.empty() || nodeName.rfind(tail) == nodeName.size() - tail.size();
                if (headMatch && tailMatch)
                    nodes.push_back(nodeIter->second);
            }
        }
        return nodes;
    }

    // -----------------------------------------------------------------------
    // environment properties
    // -----------------------------------------------------------------------

    ComputationEnvironment& Environment() const
    {
        return *m_environment;
    }

    // -----------------------------------------------------------------------
    // functions to pass on specific SGD options to nodes
    // -----------------------------------------------------------------------

    // TODO: Why are all these static, but then take a network as the first argument? --> make them class members
    static void SetDropoutRate(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode, const double dropoutRate, double& prevDropoutRate);

    static void SetIRngUserSeed(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode, size_t randSeedBase);

    template <class ElemType>
    static void SetBatchNormalizationTimeConstants(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode,
                                                   double normalizationTimeConstant, double& prevNormalizationTimeConstant,
                                                   double blendTimeConstant, double& prevBlendTimeConstant);

    template <class ElemType>
    static void SetSeqParam(ComputationNetworkPtr net,
                            const ComputationNodeBasePtr criterionNode,
                            const double& hsmoothingWeight, // TODO: Why are all these passed by reference?
                            const double& frameDropThresh,
                            const bool& doreferencealign,
                            const double& amf = 14.0f,
                            const double& lmf = 14.0f,
                            const double& wp = 0.0f,
                            const double& bMMIfactor = 0.0f,
                            const bool& sMBR = false);
    static void SetMaxTempMemSizeForCNN(ComputationNetworkPtr net, const ComputationNodeBasePtr& criterionNode, const size_t maxTempMemSizeInSamples);

    // -----------------------------------------------------------------------
    // node-group access
    // -----------------------------------------------------------------------

    // these two groups are determined from the network to be executed
    // They depend on the root node that is being evaluated.
    const std::list<ComputationNodeBasePtr>& InputNodes(const ComputationNodeBasePtr& rootNode /*, bool bNoBuild = false*/)
    {
        auto iter = m_inputValues.find(rootNode);
        if (iter == m_inputValues.end())
            LogicError("InputNodes() called for root %ls %ls operation for the set of inputs has not (yet?) been determined.", rootNode->NodeName().c_str(), rootNode->OperationName().c_str());
        return iter->second;
    }

    const std::list<ComputationNodeBasePtr>& LearnableParameterNodes(const ComputationNodeBasePtr& rootNode)
    {
        auto iter = m_learnableParameters.find(rootNode);
        if (iter == m_learnableParameters.end())
            LogicError("LearnableParameterNodes() called for root %ls %ls operation for which the set of learnable parameters has not (yet?) been determined.", rootNode->NodeName().c_str(), rootNode->OperationName().c_str());
        return iter->second;
    }

    inline const std::vector<ComputationNodeBasePtr>& CriterionNodesFrom(const wstring& criterionNodeName)
    {
        ComputationNodeBasePtr node = GetNodeFromName(criterionNodeName);
        if (node->HasMBLayout() || node->GetSampleLayout().GetNumElements() != 1)
            InvalidArgument("%ls %ls operation is not a valid training or eval criterion node.", node->NodeName().c_str(), node->OperationName().c_str());
        m_namedCriterionNodes[criterionNodeName] = std::vector<ComputationNodeBasePtr>{node};
        return m_namedCriterionNodes[criterionNodeName];
    }

    std::vector<ComputationNodeBasePtr> OutputNodesByName(const std::vector<std::wstring>& outputNodeNames)
    {
        std::vector<ComputationNodeBasePtr> outputNodes;

        if (outputNodeNames.size() == 0)
        {
            if (OutputNodes().size() == 0)
                RuntimeError("There is no default output node specified in the network.");

            outputNodes = OutputNodes();
        }
        else
        {
            for (int i = 0; i < outputNodeNames.size(); i++)
                outputNodes.push_back(GetNodeFromName(outputNodeNames[i]));
        }

        return outputNodes;
    }

    // Collect all input nodes that outputNodes depend on.
    std::vector<ComputationNodeBasePtr> InputNodesForOutputs(const std::vector<std::wstring>& outputNodeNames)
    {
        // use set to remove duplicated items
        auto outputNodes = OutputNodesByName(outputNodeNames);

        std::set<ComputationNodeBasePtr> inputNodesMap;
        for (auto& onode : outputNodes)
        {
            for (auto& inode : InputNodes(onode))
                inputNodesMap.insert(inode);
        }

        std::vector<ComputationNodeBasePtr> inputNodes;
        for (auto& inode : inputNodesMap)
            inputNodes.push_back(inode);

        return inputNodes;
    }

    std::list<ComputationNodeBasePtr> PastValueNodesForOutputs(const std::vector<ComputationNodeBasePtr>& outputNodes)
    {
        std::list<ComputationNodeBasePtr> evalOrder;
        ExecutionGraph graph(outputNodes);
        evalOrder = ::CNTK::PostOrderTraversal(graph, outputNodes);

        return evalOrder;
    }
    const std::vector<ComputationNodeBasePtr>& RootNodes() const
    {
        return m_allRoots;
    }

    // these are specified as such by the user
    const std::vector<ComputationNodeBasePtr>& FeatureNodes() const
    {
        return m_featureNodes;
    }
    const std::vector<ComputationNodeBasePtr>& LabelNodes() const
    {
        return m_labelNodes;
    }
    const std::vector<ComputationNodeBasePtr>& FinalCriterionNodes() const
    {
        return m_criterionNodes;
    }
    const std::vector<ComputationNodeBasePtr>& EvaluationNodes() const
    {
        return m_evaluationNodes;
    }
    const std::vector<ComputationNodeBasePtr>& OutputNodes() const
    {
        return m_outputNodes;
    }

private:
    // determine the node-group array by the group tag
    std::vector<ComputationNodeBasePtr>& GetNodeGroup(const std::wstring& groupTag)
    {
        if (groupTag == L"feature")
            return m_featureNodes;
        else if (groupTag == L"label")
            return m_labelNodes;
        else if (groupTag == L"criterion")
            return m_criterionNodes;
        else if (groupTag == L"evaluation")
            return m_evaluationNodes;
        else if (groupTag == L"output")
            return m_outputNodes;
        else
            InvalidArgument("Invalid group tag '%ls', must be one of 'feature', 'label', 'criterion', 'evaluation', 'output'.", groupTag.c_str());
    }

public:
    // add a node to a node group
    void AddToNodeGroup(const std::wstring& groupTag, const ComputationNodeBasePtr& node)
    {
        assert(node);

        // determine the node group by its group tag string
        auto& nodeGroup = GetNodeGroup(groupTag);
        // if node is already in the list then we are done
        if (node->HasTag(groupTag))
        {
            for (const auto& groupNode : nodeGroup) // TODO: is there an STL algorithm?
                if (groupNode == node)
                    return;
            // we get here if the node has the tag but is not in the node group yet
        }
        // verify and update the node's tag
        node->SetTag(groupTag);
        // add to the node group
        nodeGroup.push_back(node);
    }

    // remove a node from its node group
    // Returns true if the node was there.
    bool RemoveFromNodeGroup(const std::wstring& groupTag, const ComputationNodeBasePtr& node)
    {
        bool wasActuallySet = node->ClearTag(groupTag);
        if (!wasActuallySet) // if node was not member of the group, we are done
            return false;
        auto& nodeGroup = GetNodeGroup(groupTag);
        for (auto iter = nodeGroup.begin(); iter != nodeGroup.end(); iter++)
        {
            if (*iter == node)
            {
                nodeGroup.erase(iter);
                return true;
            }
        }
        LogicError("RemoveFromNodeGroup: %ls %ls operation not found in its node group '%ls'.", node->NodeName().c_str(), node->OperationName().c_str(), groupTag.c_str());
    }

    // -----------------------------------------------------------------------
    // node access
    // -----------------------------------------------------------------------

    size_t GetTotalNumberOfNodes() const
    {
        return m_nameToNodeMap.size();
    }

    std::vector<ComputationNodeBasePtr> GetAllNodes() const
    {
        std::vector<ComputationNodeBasePtr> nodes;
        for (const auto& iter : m_nameToNodeMap)
            nodes.push_back(iter.second);
        return nodes;
    }

    // determine parent map (this is needed in some editing steps)
    // Returns a map[node] -> set of parent nodes.
    std::map<ComputationNodeBasePtr, std::set<ComputationNodeBasePtr>> CreateParentsMap() const
    {
        std::map<ComputationNodeBasePtr, std::set<ComputationNodeBasePtr>> parents; // use a set because a node may have the same input multiple times, e.g. to compute x^2 as x.*x
        for (const auto& iter : m_nameToNodeMap)
        {
            const auto& node = iter.second;
            parents[node]; // make sure there is an entry for every parent
            for (const auto& child : node->GetInputs())
                parents[child].insert(node);
        }
        return parents;
    }

    // Return set of immediate output (parent) nodes for given input (child) node
    // TODO: there should be a map from output nodes to inputs, so that this operation doesn't take square time
    std::vector<ComputationNodeBasePtr> GetParentNodes(const std::wstring& inputNodeName)
    {
        std::set<ComputationNodeBasePtr> outputNodes;
        for (const auto& iter : m_nameToNodeMap)
        {
            const auto& node = iter.second;

            //Iterate over inputs of this node
            for (const auto& inputNode : node->GetInputs())
            {
                if (inputNode->GetName() == inputNodeName)
                {
                    outputNodes.insert(node);
                }
            }
        }

        return std::vector<ComputationNodeBasePtr>(outputNodes.begin(), outputNodes.end());
    }

    std::list<ComputationNodeBasePtr> GetNodesWhere(std::function<bool(const ComputationNodeBasePtr&)>& predicate, const ComputationNodeBasePtr& rootNode = nullptr) const
    {
        std::list<ComputationNodeBasePtr> filteredNodes;

        // find nodes from all available nodes
        // TODO: This distinction should not be necessary anymore. Calling GetEvalOrder(nullptr) will have the same effect.
        if (rootNode == nullptr)
        {
            for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
            {
                ComputationNodeBasePtr node = nodeIter->second;
                if (predicate(node))
                    filteredNodes.push_back(node);
            }
        }
        else
        {
            // for calculating a specific node
            for (const auto& node : GetEvalOrder(rootNode)) // TODO: verify that no use of this requires the actual eval order, then change to GetAllNodesForRoot()
            {
                if (predicate(node))
                    filteredNodes.push_back(node);
            }
        }

        return filteredNodes;
    }

    std::list<ComputationNodeBasePtr> GetNodesWithType(const wstring typeName, const ComputationNodeBasePtr& rootNode = nullptr) const
    {
        std::function<bool(const ComputationNodeBasePtr&)> predicate = [typeName](const ComputationNodeBasePtr& node) { return node->OperationName() == typeName; };
        return GetNodesWhere(predicate, rootNode);
    }

    template <typename T>
    std::list<ComputationNodeBasePtr> GetNodesWithType(const ComputationNodeBasePtr& rootNode = nullptr) const
    {
        std::function<bool(const ComputationNodeBasePtr&)> predicate = [](const ComputationNodeBasePtr& node) {
            return (dynamic_cast<T*>(node.get()) != nullptr);
        };

        return GetNodesWhere(predicate, rootNode);
    }

    // Get the eval nodes with names
    // if evalNodeNames are not specified, return all the default evalnodes and training criterion nodes.
    std::vector<ComputationNodeBasePtr> GetEvalNodesWithName(const std::vector<wstring> evalNodeNames)
    {
        // determine nodes to evaluate
        std::vector<ComputationNodeBasePtr> evalNodes;

        set<ComputationNodeBasePtr> criteriaLogged; // (keeps track ot duplicates to avoid we don't double-log criteria)
        if (evalNodeNames.size() == 0)
        {
            fprintf(stderr, "evalNodeNames are not specified, using all the default evalnodes and training criterion nodes.\n");
            if (EvaluationNodes().empty() && FinalCriterionNodes().empty())
                InvalidArgument("There is no default evaluation node or training criterion specified in the network.");

            for (const auto& node : EvaluationNodes())
                if (criteriaLogged.insert(node).second)
                    evalNodes.push_back(node);

            for (const auto& node : FinalCriterionNodes())
                if (criteriaLogged.insert(node).second)
                    evalNodes.push_back(node);
        }
        else
        {
            for (int i = 0; i < evalNodeNames.size(); i++)
            {
                const auto& node = GetNodeFromName(evalNodeNames[i]);
                if (!criteriaLogged.insert(node).second)
                    continue;
                if (node->GetSampleLayout().GetNumElements() != 1)
                    InvalidArgument("Criterion nodes to evaluate must have dimension 1x1.");
                evalNodes.push_back(node);
            }
        }

        return evalNodes;
    }

public:
    // return list of nodes that require precomputation and not precomputed yet
    std::list<ComputationNodeBasePtr> GetNodesRequiringPreComputation(const ComputationNodeBasePtr& rootNode = nullptr, bool checkComputed = true);

    // -----------------------------------------------------------------------
    // unit testing
    // -----------------------------------------------------------------------

    bool UnitTest(bool allowFragment = false);
    bool UnitTest(const ComputationNodeBasePtr& rootNode);

    // -----------------------------------------------------------------------
    // specialized operations
    // -----------------------------------------------------------------------

    template <class ElemType>
    void PerformSVDecomposition(const map<wstring, float>& SVDConfig, size_t AlignedSize);

    template <class ElemType>
    void SaveToDbnFile(ComputationNetworkPtr net, const std::wstring& fileName) const;

    // -----------------------------------------------------------------------
    // construction
    // -----------------------------------------------------------------------

protected:
// Copy constructor, should never be called.
#pragma warning(push)
#pragma warning(disable : 4702) // this function is flagged but unclear why
    ComputationNetwork(const ComputationNetwork& /*deepCopyFrom*/)
    {
        // TODO: can we just define it as private without implementation?
        LogicError("'ComputationNetwork(const ComputationNetwork& deepCopyFrom)' should never be called.");
    }
#pragma warning(pop)

    // Assignment operator, should never be called.
    ComputationNetwork& operator=(const ComputationNetwork& /*deepCopyFrom*/)
    {
        // TODO: can we just define it as private without implementation?
        LogicError("'ComputationNetwork& operator=(const ComputationNetwork& deepCopyFrom)' should never be called.");
    }

    // -----------------------------------------------------------------------
    // node creation
    // -----------------------------------------------------------------------

public:
    // TODO: move these to ComputationNetworkBuilder.cpp

    // add a node to m_nameToNodeMap[], which is our node holder
    // This only adds the node to the network's node set, without considering linkage.
    // Duplicate node names are rejected.
    ComputationNodeBasePtr AddNodeToNet(const ComputationNodeBasePtr& node)
    {
        auto result = m_nameToNodeMap.insert(make_pair(node->NodeName(), node));
        if (!result.second)
            RuntimeError("AddNodeToNet: Duplicated name for %ls %ls operation.", node->NodeName().c_str(), node->OperationName().c_str());
        node->SetEnvironment(m_environment);
        return node; // allows e.g. return AddNodeToNet(New...);
    }
    // TODO: not very nice--need to fix way more outside to get this right
    template <class N>
    shared_ptr<N> AddNodeToNetWithElemType(const shared_ptr<N> node)
    {
        return dynamic_pointer_cast<N>(AddNodeToNet(node));
    }

    template <class N>
    shared_ptr<N> AddNodeToNetAndAttachInputs(const shared_ptr<N> nodePtr, const std::vector<ComputationNodeBasePtr>& inputs)
    {
        nodePtr->AttachInputs(inputs);
        return AddNodeToNetWithElemType(nodePtr);
        // return nodePtr; // allows e.g. return AddNodeToNetAndAttachInputs(New..., inputs);
    }

    // add a node to the network unless it's already there
    // Returns false if the node was already there.
    // If the network already contains a different node with the same name,
    //  - then the function will fail
    //  - unless 'makeUniqueName=true', in which case it will patch the node's name to a unique name.
    bool AddNodeToNetIfNotYet(const ComputationNodeBasePtr& node, bool makeUniqueName = false)
    {
        auto result = m_nameToNodeMap.insert(make_pair(node->NodeName(), node));
        // if there's already one under this name, it better be node
        // unless user requested 'makeUniqueName', then we will modify the name
        while (!result.second /*if already there*/ && result.first->second != node)
        {
            if (!makeUniqueName || node->NodeName().find_first_of(L".[]") == wstring::npos)
                RuntimeError("AddNodeToNetIfNotYet: Duplicated name for %ls %ls operation (%d vs. %d).", node->NodeName().c_str(), node->OperationName().c_str(), (int) node->m_uniqueNumericId, (int) result.first->second->m_uniqueNumericId);
            node->SetName(L"_" + node->NodeName());
            result = m_nameToNodeMap.insert(make_pair(node->NodeName(), node));
        }
        node->SetEnvironment(m_environment); // (note: redundant if already part of the network)
        return result.second;
    }

    // remove a node from the network's node set
    // This does NOT update any links referencing it, or node groups.
    // TODO: We should verify that indeed this node is not referenced by other nodes or node groups,
    //       nor that this node references any node inside the network.
    ComputationNodeBasePtr RemoveNodeFromNet(const ComputationNodeBasePtr& node)
    {
        node->SetEnvironment(nullptr);
        m_nameToNodeMap.erase(node->NodeName());
        return node;
    }

public:
    // -----------------------------------------------------------------------
    // evaluation
    // -----------------------------------------------------------------------

    // zeroes out all gradients except the root itself (since its gradient is set from outside rather than propagated down)
    // (Note that inside the nodes this only really sets a flag to do it later when needed, but that's not our concern.)
    void ZeroInputGradients(const ComputationNodeBasePtr& rootNode)
    {
        for (auto& node : GetAllNodesForRoot(rootNode))
            node->ZeroGradientsOfInputs();
    }

private:
    bool IsTypicalCriterionNode(ComputationNodeBasePtr nodePtr);
    void PrintComputationTree(const ComputationNodeBasePtr& rootNode, const bool forwardCompute, const bool printMatrices = false);

public:
    // -----------------------------------------------------------------------
    // diagnostics
    // -----------------------------------------------------------------------

    void SetTrackGapNans(bool enable)
    {
        m_environment->trackGapNans = enable;
    }
    bool GetTrackGapNaNs() const
    {
        return m_environment->trackGapNans;
    }

    void SetIsV2Library(bool enable)
    {
        m_environment->isV2Library = enable;
    }
    bool GetIsV2Library() const
    {
        return m_environment->isV2Library;
    }

    void SetTraceLevel(int traceLevel)
    {
        m_environment->traceLevel = traceLevel;
    }
    int TraceLevel() const
    {
        return m_environment->traceLevel;
    }

    // call EnableNodeTracing() on the given nodes for real, category, and sparse printing
    void EnableNodeTracing(const std::vector<std::wstring>& traceNodeNamesReal,
                           const std::vector<std::wstring>& traceNodeNamesCategory,
                           const std::vector<std::wstring>& traceNodeNamesSparse)
    {
        for (const auto& name : traceNodeNamesReal)
            if (NodeNameExists(name))
                GetNodeFromName(name)->EnableNodeTracing(/*asReal=*/true, /*asCategoryLabel=*/false, /*asSparse=*/false);
            else
                fprintf(stderr, "EnableNodeTracing: No node named '%ls'; skipping\n", name.c_str());
        for (const auto& name : traceNodeNamesCategory)
            if (NodeNameExists(name))
                GetNodeFromName(name)->EnableNodeTracing(/*asReal=*/false, /*asCategoryLabel=*/true, /*asSparse=*/false);
            else
                fprintf(stderr, "EnableNodeTracing: No node named '%ls'; skipping\n", name.c_str());
        for (const auto& name : traceNodeNamesSparse)
            if (NodeNameExists(name))
                GetNodeFromName(name)->EnableNodeTracing(/*asReal=*/false, /*asCategoryLabel=*/false, /*asSparse=*/true);
            else
                fprintf(stderr, "EnableNodeTracing: No node named '%ls'; skipping\n", name.c_str());
    }

    // if node name is not found, dump all nodes
    // otherwise dump just that node
    // This function is called from MEL, i.e. must be prepared to operate on an uncompiled network (only m_nameToNodeMap is valid).
    void DumpNodeInfoToFile(const std::wstring& nodeName, const bool printValues, const bool printMetadata, const std::wstring outputFile, const std::wstring& nodeNameInRegEx = L"")
    {
        if (nodeNameInRegEx.empty())
        {
            if (NodeNameExists(nodeName))
            {
                File fstream(outputFile,
                             FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

                const ComputationNodeBasePtr& nodePtr = GetNodeFromName(nodeName);
                nodePtr->DumpNodeInfo(printValues, printMetadata, fstream);
            }
            else // node name is not found, dump all nodes
            {
                fprintf(stderr, "Warning: node name '%ls' does not exist in the network. dumping all nodes instead.\n",
                        nodeName.c_str());
                DumpAllNodesToFile(printValues, printMetadata, outputFile);
            }
        }
        else
        {
            std::wregex NameRegEx(nodeNameInRegEx);
            std::vector<ComputationNodeBasePtr> NodeList;
            std::vector<wstring> NameList;
            for (auto m : m_nameToNodeMap)
            {
                if (regex_match(m.first, NameRegEx))
                {
                    NodeList.push_back(m.second);
                    NameList.push_back(m.first);
                }
            }
            fprintf(stderr, "DumpNodeInfo: %d nodes matching RegEx(%ls): \n", (int) NameList.size(), nodeNameInRegEx.c_str());
            for (auto x : NameList)
            {
                fprintf(stderr, "\t%ls\n", x.c_str());
            }
            fprintf(stderr, "DumpNodeInfo: dumping node info (%s printing values) to %ls\n", printValues ? "with" : "without", outputFile.c_str());
            DumpNodeInfoToFile(NodeList, printValues, printMetadata, outputFile);
        }
    }

    // dump all nodes in the network to file
    void DumpAllNodesToFile(const bool printValues,
                            const bool printMetadata,
                            const std::wstring outputFile)
    {
        File fstream(outputFile,
                     FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

        for (auto nodeIter = m_nameToNodeMap.begin(); nodeIter != m_nameToNodeMap.end(); nodeIter++)
        {
            ComputationNodeBasePtr nodePtr = nodeIter->second;
            nodePtr->DumpNodeInfo(printValues, printMetadata, fstream);
        }
    }

    // this one is called from MEL and from DumpNodeInfoToFile() above
    void DumpNodeInfoToFile(const vector<ComputationNodeBasePtr>& nodes,
                            const bool printValues,
                            const bool printMetadata,
                            const std::wstring outputFile)
    {
        File fstream(outputFile,
                     FileOptions::fileOptionsText | FileOptions::fileOptionsWrite);

        for (auto nodeIter = nodes.begin(); nodeIter != nodes.end(); nodeIter++)
        {
            ComputationNodeBasePtr nodePtr = *nodeIter;
            nodePtr->DumpNodeInfo(printValues, printMetadata, fstream);
        }
    }

    // -----------------------------------------------------------------------
    // topological plot [1/13/2015 erw] plot network topology using dot language
    // -----------------------------------------------------------------------

private:
    wstring FormSpecialNodes(wstring style, std::vector<ComputationNodeBasePtr>& specialNodes);
    typedef std::pair<ComputationNodeBasePtr, ComputationNodeBasePtr> ComputationArc;

public:
    void DescribeNetworkUsingDot(std::list<ComputationArc>& arcs, std::wstring outFile);
    void PlotNetworkTopology(const std::wstring& outputFile);

    // -----------------------------------------------------------------------
    // scripting integration
    // -----------------------------------------------------------------------

    // pretend to be a ConfigRecord
    void /*CustomConfigRecord::*/ LazyCreateConfigMember(const wstring& id) const override;
    vector<wstring> /*IConfigRecord::*/ GetMemberIds() const override;

    // create a somewhat readable representation, aimed at diagnostics/debugging
    wstring /*HasToString::*/ ToString() const
    {
        wstring args;
        for (auto& iter : m_nameToNodeMap)
        {
            const auto node = iter.second;
            if (!args.empty())
                args.append(L"\n");
            args.append(node->ToString());
        }
        return TypeId<decltype(*this)>() + L" " + NestString(args, L'[', true, ']');
    }

protected:
    // FlowControlNodes for internal use by this class:

    // -----------------------------------------------------------------------
    // SEQTraversalFlowControlNode -- FlowControlNode to traverse a (sub-)network time step by time step
    //
    // This is to implement recurrent loops. All nodes inside a loop are listed
    // inside this node. This node's ForwardProp() function will execute
    // them inside a loop over all time steps of the recurrence.
    // For every time step, the entire chain of nodes is called, with the time index
    // passed as a FrameRange object.
    // -----------------------------------------------------------------------

    class SEQTraversalFlowControlNode : public FlowControlNode
    {
    public: // m_nestedNodes needed public by ComputationNetwork::FindInRecurrentLoops(), which really should be part of SEQTraversalFlowControlNode
        typedef FlowControlNode Base;
        using Base::m_nestedNodes;

    public:
        virtual const std::wstring OperationName() const override
        {
            return L"SEQTraversalFlowControlNode";
        }
        virtual void BeginForwardProp() override;
        virtual void ForwardProp(const FrameRange&) override;
        virtual void EndForwardProp() override;
        virtual void BeginBackprop() override;
        virtual void BackpropTo(const size_t inputIndex, const FrameRange&) override
        {
            NOT_IMPLEMENTED;
        }
        virtual void EndBackprop() override;
        virtual void Backprop(const FrameRange& fr, bool childrenInThisLoop, bool childrenInOuterLoop) override;
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool);
        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool);
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool);
        virtual bool IsOutOfDateWrtInputs() const override;

    public:
        ComputationNodeBasePtr m_sourceNode; // one of the nodes of the loop   --TODO: What is the special meaning of this node? It seems to always be a delay node.
        int m_loopId;                        // unique loop id, index in m_allSEQNodes array
        int m_steppingDirection;             // +1 if left to right (t=0..T-1), -1 if rightt to left (t=T-1..0)

        SEQTraversalFlowControlNode(int loopId, ComputationNodeBasePtr cur)
            : m_loopId(loopId),
              m_sourceNode(cur)
        {
            SetNodeName(L"Loop_" + m_sourceNode->NodeName());
        }
    };

    // -----------------------------------------------------------------------
    // PARTraversalFlowControlNode -- FlowControlNode that traverses a (sub-)network
    //
    // This node contains a list of nodes in a (sub-)network. This node's
    // ForwardProp() method will execute all those nodes once in PAR mode,
    // that is, by passing a FrameRange object that represents to operate
    // on all frames in the node simultaneously.
    //
    // The outermost network level is also represented by this node for execution.
    // -----------------------------------------------------------------------

    class PARTraversalFlowControlNode : public FlowControlNode
    {
        typedef FlowControlNode Base;
        using Base::m_nestedNodes;

    public:
        virtual const std::wstring OperationName() const override
        {
            return L"PARTraversalFlowControlNode";
        }

        static void ForwardProp(const ComputationNodeBasePtr& node, const FrameRange& fr);
        static void PostForwardAndBackProp(const ComputationNodeBasePtr& node);

        virtual void BeginForwardProp() override {}
        virtual void ForwardProp(const FrameRange&) override;
        virtual void EndForwardProp() override {}

        virtual void PostForwardAndBackProp() override;

        virtual void BeginBackprop() override {}
        virtual void BackpropTo(const size_t inputIndex, const FrameRange&) override
        {
            NOT_IMPLEMENTED;
        } // ugh, call Backprop() instead
        virtual void EndBackprop() override {}

        virtual void Backprop(const FrameRange& fr, bool childrenInThisLoop, bool childrenInOuterLoop) override;
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool);
        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool);
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool);
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool);

    public:
        // this special constructor constructs the top-level network node
        // There is currently no other constructor for inner nested PAR-traversed sub-networks, but there will be.
        PARTraversalFlowControlNode(const std::vector<shared_ptr<SEQTraversalFlowControlNode>>& recurrentInfo, const std::list<ComputationNodeBasePtr>& allNodes);
        // Base::m_nestedNodes contains all top-level nodes, in evaluation order
    };

public:
    // -----------------------------------------------------------------------
    // data members
    // -----------------------------------------------------------------------

    unsigned long GetRandomSeedOffset() const
    {
        return m_randomSeedOffset;
    }
    void SetRandomSeedOffset(unsigned long value)
    {
        m_randomSeedOffset = value;
    }

private:
    DEVICEID_TYPE m_deviceId; // TODO: is this shared by all nodes?
    unsigned long m_randomSeedOffset;

    // main node holder
    std::map<const std::wstring, ComputationNodeBasePtr, nocase_compare> m_nameToNodeMap; // [name] -> node; this is the main container that holds this networks' nodes

    // node groups
    // These are specified by the user by means of tags or explicitly listing the node groups.
    // TODO: Are these meant to be disjoint?
    std::vector<ComputationNodeBasePtr> m_featureNodes;             // tag="feature"
    std::vector<ComputationNodeBasePtr> m_labelNodes;               // tag="label"
    std::vector<ComputationNodeBasePtr> m_criterionNodes;           // tag="criterion"
    std::vector<ComputationNodeBasePtr> m_evaluationNodes;          // tag="evaluation"
    std::vector<ComputationNodeBasePtr> m_outputNodes;              // tag="output"
    vector<std::vector<ComputationNodeBasePtr>*> GetAllNodeGroups() // get all groups to allow to iterate over all of them ...continue
    {
        return vector<std::vector<ComputationNodeBasePtr>*>{&m_featureNodes, &m_labelNodes, &m_criterionNodes, &m_evaluationNodes, &m_outputNodes};
    }

    // used for sentence boundary information passed from reader to reset RNN state
    // specify how the minibatch is packed for each sample
    // BUGBUG (Issue #95): With variable-length inconsistent layouts, this can no longer be a network property.
    MBLayoutPtr m_pMBLayoutOfNetwork; // note that this must be installed before doing anything that needs it (default leaves a nullptr)

    // environment information that nodes may want to inquire, e.g. to know whether we are training
    ComputationEnvironmentPtr m_environment;

    std::map<std::wstring, std::vector<ComputationNodeBasePtr>> m_namedCriterionNodes;

private:
    // -----------------------------------------------------------------------
    // the following members are all result of post-processing by CompileNetwork()
    // -----------------------------------------------------------------------

    // list of all roots in this network
    // A root is a node that can run as a target of ForwardProp(). See DetermineSetOfAllRoots().
    std::vector<ComputationNodeBasePtr> m_allRoots;

    std::vector<std::shared_ptr<SEQTraversalFlowControlNode>> m_allSEQNodes; // [loopId] cached set of SEQTraversalFlowControlNodes to allow sharing and idempotence of FormRecurrentLoops()

    // cache for evaluation ordering:
    bool m_isCompiled;           // CompileNetwork has been called
    bool m_areMatricesAllocated; // AllocateAllMatrices has been called

    // cached network iterations
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_evalOrders; // [out node] flat depth-first traversal starting from out node
    std::map<const ComputationNodeBasePtr, ComputationNodeBasePtr> m_nestedNetworks;        // [out node] network rewritten as recursive traveral, potentially optimized; execution plan

    // cached quick-access list for inputs and parameters
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_inputValues;         // [out node] -> all input nodes feeding into out node
    std::map<const ComputationNodeBasePtr, std::list<ComputationNodeBasePtr>> m_learnableParameters; // [out node] -> all parameter nodes feeding into out node

private:
    // pool for matrices that can be shared across nodes
    // TODO: does this apply to anything else besides temporary node-internal intermediate results? What, for example?
    MatrixPool m_matrixPool;

    // Implementation of a graph based on ComputationNodes.
    class ExecutionGraph : public ::CNTK::DirectedGraph<ComputationNodeBasePtr>
    {
        std::vector<ComputationNodeBasePtr> m_roots;

    public:
        ExecutionGraph(const std::vector<ComputationNodeBasePtr>& roots)
            : m_roots(roots) {}

        std::vector<ComputationNodeBasePtr> Predecessors(const ComputationNodeBasePtr& node) const override
        {
            return node->GetInputs();
        }

        const std::vector<ComputationNodeBasePtr>& Roots() const override
        {
            return m_roots;
        }
    };
}; // namespace CNTK
typedef ComputationNetwork::ComputationNetworkPtr ComputationNetworkPtr;

class DataReaderHelpersFunctions
{
public:
    /* start:  move from datareaderhelpers to here */
    template <class ElemType>
    static void NotifyChangedNodes(ComputationNetworkPtr net, StreamMinibatchInputs& inputMatrices)
    {
        // reader will have resized input node's m_value directly. Nodes must be notified to do necessary internal state updates from that.
        // TODO: This is a stopgap. SGD will at some point change from sets of matrices to sets of nodes. Then this will become much simpler.
        std::set<MatrixBasePtr> matrices;
        for (const auto& iter : inputMatrices)
            matrices.insert(iter.second.matrix);
        for (auto& node : net->FeatureNodes())
            if (matrices.find(node->As<ComputationNode<ElemType>>()->ValuePtr()) != matrices.end())
                node->NotifyFunctionValuesMBSizeModified();
        for (auto& node : net->LabelNodes())
            if (matrices.find(node->As<ComputationNode<ElemType>>()->ValuePtr()) != matrices.end())
                node->NotifyFunctionValuesMBSizeModified();
    }

    // get StreamMinibatchInputs for a given set of input nodes
    static StreamMinibatchInputs RetrieveInputMatrices(const std::vector<ComputationNodeBasePtr>& inputNodes)
    {
        StreamMinibatchInputs inputMatrices;
        for (auto& node : inputNodes)
            inputMatrices.AddInput(node->NodeName(), node->ValuePtr(), node->GetMBLayout(), node->GetSampleLayout());
        return inputMatrices;
    }
    /* end:  move from datareaderhelpers to here */
};

class WERFunctions
{
    public:
    void convert_word_sequence_string_2_vector(string word_sequence, vector<string>& vt_words, char separator)
    {
        if (word_sequence == "")
            return;
        size_t lp, rp;
        rp = 0;
        while (true)
        {
            lp = word_sequence.find_first_not_of(separator, rp);
            rp = word_sequence.find_first_of(separator, lp);
            if (rp == string::npos)
            {
                vt_words.push_back(word_sequence.substr(lp));
                break;
            }
            else
                vt_words.push_back(word_sequence.substr(lp, rp - lp));
        }
    }

    float compute_wer(const vector<string>& ref, vector<string>& rec)
    {
        short** mat;
        size_t i, j;

        mat = new short*[rec.size() + 1];
        for (i = 0; i <= rec.size(); i++)
            mat[i] = new short[ref.size() + 1];

        for (i = 0; i <= rec.size(); i++)
            mat[i][0] = short(i);
        for (j = 1; j <= ref.size(); j++)
            mat[0][j] = short(j);

        for (i = 1; i <= rec.size(); i++)
            for (j = 1; j <= ref.size(); j++)
            {
                mat[i][j] = mat[i - 1][j - 1];

                if (rec[i - 1] != ref[j - 1])
                {

                    if ((mat[i - 1][j]) < mat[i][j])
                        mat[i][j] = mat[i - 1][j];
                    if ((mat[i][j - 1]) < mat[i][j])
                        mat[i][j] = mat[i][j - 1];
                    mat[i][j]++;
                }
            }
        float wer = float(mat[rec.size()][ref.size()]) / ref.size();
        for (i = 0; i <= rec.size(); i++)
            delete[] mat[i];
        delete[] mat;
        return wer;
    }

};
template <class ElemType>

class RNNTDecodeFunctions
{
public:
    unordered_map<wstring, vector<shared_ptr<PastValueNode<ElemType>>>> m_nameToPastValueNodeCache;
    vector<shared_ptr<Matrix<ElemType>>> m_decodeOutputCache;
    std::vector<wstring> m_nodesToCache;

    struct Sequence
    {
        //shared_ptr<Matrix<ElemType>> LabelMatrix;
        std::vector<size_t> labelseq;
        ElemType logP;
        size_t length;
        size_t processlength;
        size_t lengthwithblank;
        shared_ptr<Matrix<ElemType>> decodeoutput;
        bool operator<(const Sequence& rhs) const
        {
            return logP < rhs.logP;
        }
        bool realValues = false;
        unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>> nameToParentNodeValues;
        unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>> nameToNodeValues;
        long refs = 0;
    };

    Sequence newSeq(size_t numRow, size_t numCol, DEVICEID_TYPE deviceId)
    {
        Sequence oneSeq = {std::vector<size_t>(), 0.0, 0, 0, 0, make_shared<Matrix<ElemType>>(numRow, (size_t) 1, deviceId)};

        for (size_t i = 0; i < m_nodesToCache.size(); i++)
        {
            vector<ElemType> v;
            oneSeq.nameToNodeValues[m_nodesToCache[i]] = make_shared<PastValueNode<ElemType>>(deviceId, m_nodesToCache[i]);
        }

        return oneSeq;
    }

    Sequence newSeq(Sequence& a, DEVICEID_TYPE deviceId)
    {
        Sequence oneSeq;
        oneSeq.labelseq = a.labelseq;
        oneSeq.logP = a.logP;
        oneSeq.length = a.length;
        oneSeq.lengthwithblank = a.lengthwithblank;
        oneSeq.processlength = a.processlength;
        if (m_decodeOutputCache.size() > 0)
        {
            oneSeq.decodeoutput = m_decodeOutputCache.back();
            m_decodeOutputCache.pop_back();
        }
        else
        {
            oneSeq.decodeoutput = make_shared<Matrix<ElemType>>(a.decodeoutput->GetNumRows(), (size_t) 1, a.decodeoutput->GetDeviceId());
        }
        oneSeq.decodeoutput->SetValue(*(a.decodeoutput));

        typename unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>>::iterator it;
        for (it = a.nameToNodeValues.begin(); it != a.nameToNodeValues.end(); it++)
        {
            if (oneSeq.processlength > 0)
            {
                if (it->second->Value().GetNumElements() > 0 && a.realValues)
                {
                    oneSeq.nameToParentNodeValues[it->first] = it->second;
                    a.refs++;
                }
                else
                    oneSeq.nameToParentNodeValues[it->first] = a.nameToParentNodeValues[it->first];
                /*size_t ab = oneSeq.nameToParentNodeValues[it->first]->Value().GetNumElements();
                if (ab > 0)
                    fprintf(stderr, "test %ls %zu", it->first.c_str(), ab);*/
            }
            auto itin = m_nameToPastValueNodeCache.find(it->first);
            if (itin != m_nameToPastValueNodeCache.end() && m_nameToPastValueNodeCache[it->first].size() > 0)
            {
                oneSeq.nameToNodeValues[it->first] = m_nameToPastValueNodeCache[it->first].back();
                m_nameToPastValueNodeCache[it->first].pop_back();
            }
            else
            {
                oneSeq.nameToNodeValues[it->first] = make_shared<PastValueNode<ElemType>>(deviceId, it->first);
            }
            /*std::ostringstream address;
            address << oneSeq.nameToNodeValues[it->first];
            fprintf(stderr, "newSeq %ls %s \n", it->first.c_str(), address.str().c_str());*/
        }

        return oneSeq;
    }

    void deleteSeq(Sequence oneSeq)
    {
        typename unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>>::iterator it;
        for (it = oneSeq.nameToNodeValues.begin(); it != oneSeq.nameToNodeValues.end(); it++)
        {
            auto itin = m_nameToPastValueNodeCache.find(it->first);
            if (itin == m_nameToPastValueNodeCache.end())
                m_nameToPastValueNodeCache[it->first] = vector<shared_ptr<PastValueNode<ElemType>>>();
            if (oneSeq.refs == 0)
                m_nameToPastValueNodeCache[it->first].push_back(oneSeq.nameToNodeValues[it->first]);
        }
        m_decodeOutputCache.push_back(oneSeq.decodeoutput);

        vector<size_t>().swap(oneSeq.labelseq);
    }

    // the below 2 newseq and deleteseq are for multithread, where different thread do not want to share the m_nameToPastValueNodeCache and m_decodeOutputCache. Rather, each thread will have its own passed explicitly by parameter ;

    Sequence newSeq(Sequence& a, DEVICEID_TYPE deviceId, unordered_map<wstring, vector<shared_ptr<PastValueNode<ElemType>>>>& m_nameToPastValueNodeCachePerThread, vector<shared_ptr<Matrix<ElemType>>>& m_decodeOutputCachePerThread)
    {
        Sequence oneSeq;
        oneSeq.labelseq = a.labelseq;
        oneSeq.logP = a.logP;
        oneSeq.length = a.length;
        oneSeq.lengthwithblank = a.lengthwithblank;
        oneSeq.processlength = a.processlength;
        if (m_decodeOutputCachePerThread.size() > 0)
        {
            oneSeq.decodeoutput = m_decodeOutputCachePerThread.back();
            m_decodeOutputCachePerThread.pop_back();
        }
        else
        {
            oneSeq.decodeoutput = make_shared<Matrix<ElemType>>(a.decodeoutput->GetNumRows(), (size_t) 1, a.decodeoutput->GetDeviceId());
        }
        oneSeq.decodeoutput->SetValue(*(a.decodeoutput));

        typename unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>>::iterator it;
        for (it = a.nameToNodeValues.begin(); it != a.nameToNodeValues.end(); it++)
        {
            if (oneSeq.processlength > 0)
            {
                if (it->second->Value().GetNumElements() > 0 && a.realValues)
                {
                    oneSeq.nameToParentNodeValues[it->first] = it->second;
                    a.refs++;
                }
                else
                    oneSeq.nameToParentNodeValues[it->first] = a.nameToParentNodeValues[it->first];
                /*size_t ab = oneSeq.nameToParentNodeValues[it->first]->Value().GetNumElements();
                if (ab > 0)
                    fprintf(stderr, "test %ls %zu", it->first.c_str(), ab);*/
            }
            auto itin = m_nameToPastValueNodeCachePerThread.find(it->first);
            if (itin != m_nameToPastValueNodeCachePerThread.end() && m_nameToPastValueNodeCachePerThread[it->first].size() > 0)
            {
                oneSeq.nameToNodeValues[it->first] = m_nameToPastValueNodeCachePerThread[it->first].back();
                m_nameToPastValueNodeCachePerThread[it->first].pop_back();
            }
            else
            {
                oneSeq.nameToNodeValues[it->first] = make_shared<PastValueNode<ElemType>>(deviceId, it->first);
            }
            /*std::ostringstream address;
            address << oneSeq.nameToNodeValues[it->first];
            fprintf(stderr, "newSeq %ls %s \n", it->first.c_str(), address.str().c_str());*/
        }

        return oneSeq;
    }

    void deleteSeq(Sequence oneSeq, unordered_map<wstring, vector<shared_ptr<PastValueNode<ElemType>>>>& m_nameToPastValueNodeCachePerThread, vector<shared_ptr<Matrix<ElemType>>>& m_decodeOutputCachePerThread)
    {
        typename unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>>::iterator it;
        for (it = oneSeq.nameToNodeValues.begin(); it != oneSeq.nameToNodeValues.end(); it++)
        {
            auto itin = m_nameToPastValueNodeCachePerThread.find(it->first);
            if (itin == m_nameToPastValueNodeCachePerThread.end())
                m_nameToPastValueNodeCachePerThread[it->first] = vector<shared_ptr<PastValueNode<ElemType>>>();
            if (oneSeq.refs == 0)
                m_nameToPastValueNodeCachePerThread[it->first].push_back(oneSeq.nameToNodeValues[it->first]);
        }
        m_decodeOutputCachePerThread.push_back(oneSeq.decodeoutput);

        vector<size_t>().swap(oneSeq.labelseq);
    }

    void extendSeq(Sequence& insequence, size_t labelId, ElemType logP)
    {
        insequence.labelseq.push_back(labelId);
        insequence.logP = logP;
        insequence.length++;
        insequence.lengthwithblank++;
    }
    vector<pair<size_t, ElemType>> getTopN(Microsoft::MSR::CNTK::Matrix<ElemType>& prob, size_t N, const size_t& blankid)
    {
        vector<pair<size_t, ElemType>> datapair;
        typedef typename vector<pair<size_t, ElemType>>::value_type ValueType;
        ElemType* probdata = prob.CopyToArray();
        for (size_t n = 0; n < prob.GetNumRows(); n++)
        {
            datapair.push_back(ValueType(n, probdata[n]));
        }
        nth_element(datapair.begin(), datapair.begin() + N, datapair.end(), [](ValueType const& x, ValueType const& y) -> bool {
            return y.second < x.second;
        });
        datapair.push_back(ValueType(blankid, probdata[blankid]));
        delete probdata;
        return datapair;
    }

    void prepareSequence(Sequence& s)
    {
        if (s.nameToNodeValues.size() > 0)
        {
            typename unordered_map<wstring, shared_ptr<PastValueNode<ElemType>>>::iterator it;
            for (it = s.nameToParentNodeValues.begin(); it != s.nameToParentNodeValues.end(); it++)
            {
                if (it->second && it->second->Value().GetNumElements() > 0)
                {
                    it->second->CopyTo(s.nameToNodeValues[it->first], it->first, CopyNodeFlags::copyNodeAll);
                    /*std::ostringstream address;
                address << s.nameToNodeValues[it->first];
                    fprintf(stderr, "prepareSequence %ls %s \n", it->first.c_str(), address.str().c_str());*/
                }
            }
        }
        s.realValues = true;
    }

    void forward_decode(Sequence& oneSeq, StreamMinibatchInputs decodeinputMatrices, DEVICEID_TYPE deviceID, const std::vector<ComputationNodeBasePtr>& decodeOutputNodes,
                        const std::vector<ComputationNodeBasePtr>& decodeinputNodes, size_t vocabSize, size_t plength, ComputationNetwork& net, int uttFrameNum = 0)

    {

        //        size_t labelLength = oneSeq.length;
        if (oneSeq.processlength + 1 != plength && plength != oneSeq.processlength)
            LogicError("Current implementation assumes 1 step difference");
        /*
        if (uttFrameNum == 94)
        {
            for (const auto& node : net.GetAllNodesForRoot(decodeOutputNodes[0]))
            {

                if (dynamic_pointer_cast<ComputationNode<ElemType>>(node)->Value().IsEmpty())
                {
                    fprintf(stderr, "forward_decode 0  NodeName = %ls, Empty \n", node->NodeName().c_str());
                }
                else
                {
                    double pnorm = dynamic_pointer_cast<ComputationNode<ElemType>>(node)->Value().FrobeniusNorm();
                    fprintf(stderr, "forward_decode 0  NodeName = %ls, Norm = %f \n", node->NodeName().c_str(), pnorm);
                }
            }
        }
        */
        if (plength != oneSeq.processlength)
        {
            Matrix<ElemType> lmin(deviceID);

            lmin.Resize(vocabSize, 1);
            lmin.SetValue(0.0);
            lmin(oneSeq.labelseq[plength - 1], 0) = 1.0;
            auto lminput = decodeinputMatrices.begin();
            if (lminput->second.pMBLayout == NULL)
            {
                lminput->second.pMBLayout = make_shared<MBLayout>();
            }
            lminput->second.pMBLayout->Init(1, 1);
            //std::swap(lminput->second.GetMatrix<ElemType>(), lmin);
            lminput->second.GetMatrix<ElemType>().SetValue(lmin);
            if (plength == 1)
            {
                lminput->second.pMBLayout->AddSequence(NEW_SEQUENCE_ID, 0, 0, 1);
            }
            else
            {
                ///lminput->second.pMBLayout->//m_sequences.erase(0);
                lminput->second.pMBLayout->AddSequence(NEW_SEQUENCE_ID, 0, SentinelValueIndicatingUnspecifedSequenceBeginIdx, 1);

                //DataReaderHelpers::NotifyChangedNodes<ElemType>(m_net, decodeinputMatrices);

                for (size_t i = 0; i < m_nodesToCache.size(); i++)
                {
                    auto nodePtr = net.GetNodeFromName(m_nodesToCache[i]);

                    if (oneSeq.nameToNodeValues[m_nodesToCache[i]]->Value().GetNumElements() > 0)

                    {
                        oneSeq.nameToNodeValues[m_nodesToCache[i]]->CopyTo(nodePtr, m_nodesToCache[i], CopyNodeFlags::copyNodeInputLinks);
                    }
                }
            }

            net.BumpEvalTimeStamp(decodeinputNodes);
            // NotifyChangedNodes<ElemType>(m_net, decodeinputMatrices);

            net.ForwardProp(decodeOutputNodes[0]);
            /*
                if (uttFrameNum == 94)
                {

                    for (const auto& node : net.GetAllNodesForRoot(decodeOutputNodes[0]))
                    {

                        if (dynamic_pointer_cast<ComputationNode<ElemType>>(node)->Value().IsEmpty())
                        {
                            fprintf(stderr, "forward_decode 1  NodeName = %ls, Empty \n", node->NodeName().c_str());
                        }
                        else
                        {
                            double pnorm = dynamic_pointer_cast<ComputationNode<ElemType>>(node)->Value().FrobeniusNorm();
                            fprintf(stderr, "forward_decode 1  NodeName = %ls, Norm = %f \n", node->NodeName().c_str(), pnorm);
                        }
                    }
                    fprintf(stderr, "forward_decode decodeOutputNodes = %f, oneSeq.decodeoutput = %f, debug 1\n", (*(&dynamic_pointer_cast<ComputationNode<ElemType>>(decodeOutputNodes[0])->Value())).FrobeniusNorm(), (*(oneSeq.decodeoutput)).FrobeniusNorm());
                }
                */
            //Matrix<ElemType> tempMatrix = *(&dynamic_pointer_cast<ComputationNode<ElemType>>(decodeOutputNodes[0])->Value());
            oneSeq.decodeoutput->SetValue((*(&dynamic_pointer_cast<ComputationNode<ElemType>>(decodeOutputNodes[0])->Value())));
            // fprintf(stderr, "forward_decode = %f \n", oneSeq.decodeoutput->FrobeniusNorm());
            /*
                if (uttFrameNum == 94)
                {
                    fprintf(stderr, "oneSeq.decodeoutput = %f, debug 2\n", (*(oneSeq.decodeoutput)).FrobeniusNorm());
                }
                */
            oneSeq.processlength = plength;

            for (size_t i = 0; i < m_nodesToCache.size(); i++)
            {
                auto nodePtr = net.GetNodeFromName(m_nodesToCache[i]);

                if (plength == 1)
                {
                    nodePtr->CopyTo(oneSeq.nameToNodeValues[m_nodesToCache[i]], m_nodesToCache[i], CopyNodeFlags::copyNodeAll);
                }
            }

            lmin.ReleaseMemory();
        }
    }
    void forwardmerged(Sequence a, size_t t, const Matrix<ElemType>& encodeOutput, Matrix<ElemType>& decodeOutput,
                       std::vector<ComputationNodeBasePtr> Plusnodes, std::vector<ComputationNodeBasePtr> Plustransnodes, const Matrix<ElemType>& Wm, const Matrix<ElemType>& bm, const ComputationNetworkPtr& net,
                       int uttFrameNum = 0, DEVICEID_TYPE deviceID = CPUDEVICE)
    {
        /*
        if (uttFrameNum == 94)
        {

            fprintf(stderr, "frowardmerged encodeoutput = %f, a.decodeoutput = %f, debug 1\n", encodeOutput.ColumnSlice(t, 1).FrobeniusNorm(), (*(a.decodeoutput)).FrobeniusNorm());
        }
        */
        decodeOutput.AssignSumOf(encodeOutput.ColumnSlice(t, 1), *(a.decodeoutput)); // sum broadcast
        //decodeOutput.AssignSumOf(encodeOutput.ColumnSlice(t, 1), encodeOutput.ColumnSlice(t, 1));
        /*
        if (uttFrameNum == 94)
        {
            fprintf(stderr, "frowardmerged decodeOutput = %f, debug 2\n", decodeOutput.FrobeniusNorm());
        }
        */

        Matrix<ElemType> tempMatrix(deviceID);
        //plus broadcast
        if (!net)
        {
            //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 3 \n", uttNum, (void*) (&decodeOutput));
            decodeOutput.SetToZeroIfLessThan(0); // reLU
            //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 4 \n", uttNum, (void*) (&decodeOutput));
            /*
            if (uttFrameNum == 94)
            {
                fprintf(stderr, "frowardmerged decodeOutput = %f, debug 3\n", decodeOutput.FrobeniusNorm());
            }
            */
        }
        else
        {
            (&dynamic_pointer_cast<ComputationNode<ElemType>>(Plusnodes[0])->Value())->SetValue(decodeOutput);
            ComputationNetwork::BumpEvalTimeStamp(Plusnodes);
            auto PlusMBlayout = Plusnodes[0]->GetMBLayout();
            PlusMBlayout->Init(1, 1);
            PlusMBlayout->AddSequence(NEW_SEQUENCE_ID, 0, 0, 1);

            net->ForwardPropFromTo(Plusnodes, Plustransnodes);
            decodeOutput.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(Plustransnodes[0])->Value()));
        }

        //fprintf(stderr, "forward merge = %f \n", decodeOutput.FrobeniusNorm());
        //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 5 \n", uttNum, (void*) (&decodeOutput));
        /*
        if (uttFrameNum == 94)
        {

            fprintf(stderr, "frowardmerged Wm = %f, decodeoutput = %f, debug 4\n", Wm.FrobeniusNorm(), decodeOutput.FrobeniusNorm());
        }
        */
        tempMatrix.AssignProductOf(Wm, true, decodeOutput, false);
        //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 6 \n", uttNum, (void*) (&decodeOutput));
        /*
        if (uttFrameNum == 94)
        {

            fprintf(stderr, "frowardmerged bm = %f, tempMatrix = %f, debug 5\n", bm.FrobeniusNorm(), tempMatrix.FrobeniusNorm());
        }
        */
        decodeOutput.AssignSumOf(tempMatrix, bm);
        /*
        if (uttFrameNum == 94)
        {

            fprintf(stderr, "frowardmerged decodeOutput = %f, debug 6\n", decodeOutput.FrobeniusNorm());
        }
        */
        //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 7 \n", uttNum, (void*) (&decodeOutput));
        //decodeOutput.VectorMax(maxIdx, maxVal, true);
        decodeOutput.InplaceLogSoftmax(true);
        /*
        if (uttFrameNum == 94)
        {

            fprintf(stderr, "frowardmerged decodeOutput = %f, debug 7\n", decodeOutput.FrobeniusNorm());
        }
        */
        //fprintf(stderr, "debug forwardmerge uttNum = %d, &decodeOutput = %p , 8 \n", uttNum, (void*) (&decodeOutput));
    }

    void forwardmergedSVD(Sequence a, size_t t, const Matrix<ElemType>& encodeOutput, Matrix<ElemType>& decodeOutput, std::vector<ComputationNodeBasePtr> Plusnodes, std::vector<ComputationNodeBasePtr> Plustransnodes, const Matrix<ElemType>& Wmu, const Matrix<ElemType>& Wmv, const Matrix<ElemType>& bm, const ComputationNetworkPtr& net)
    {

        decodeOutput.AssignSumOf(encodeOutput.ColumnSlice(t, 1), *(a.decodeoutput));
        Matrix<ElemType> tempMatrix(encodeOutput.GetDeviceId()), tempMatrix1(encodeOutput.GetDeviceId()); //broadcast
        //plus broadcast

        if (!net)
        {
            decodeOutput.SetToZeroIfLessThan(0); //reLu
        }
        else
        {
            (&dynamic_pointer_cast<ComputationNode<ElemType>>(Plusnodes[0])->Value())->SetValue(decodeOutput);
            //SumMatrix.SetValue(sumofENandDE);
            ComputationNetwork::BumpEvalTimeStamp(Plusnodes);
            auto PlusMBlayout = Plusnodes[0]->GetMBLayout();
            PlusMBlayout->Init(1, 1);
            PlusMBlayout->AddSequence(NEW_SEQUENCE_ID, 0, 0, 1);

            net->ForwardPropFromTo(Plusnodes, Plustransnodes);
            decodeOutput.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(Plustransnodes[0])->Value()));
        }

        // fprintf(stderr, "forward merge SVD = %f \n", decodeOutput.FrobeniusNorm());
        tempMatrix.AssignProductOf(Wmu, true, decodeOutput, false);
        tempMatrix1.AssignProductOf(Wmv, true, tempMatrix, false);
        decodeOutput.AssignSumOf(tempMatrix1, bm);
        //decodeOutput.VectorMax(maxIdx, maxVal, true);
        decodeOutput.InplaceLogSoftmax(true);
    }
    /*
    void convert_word_sequence_string_2_vector(string word_sequence, vector<string>& vt_words, char separator)
    {
        if (word_sequence == "")
            return;
        size_t lp, rp;
        rp = 0;
        while (true)
        {
            lp = word_sequence.find_first_not_of(separator, rp);
            rp = word_sequence.find_first_of(separator, lp);
            if (rp == string::npos)
            {
                vt_words.push_back(word_sequence.substr(lp));
                break;
            }
            else
                vt_words.push_back(word_sequence.substr(lp, rp - lp));
        }
    }

    float compute_wer(const vector<string>& ref, vector<string>& rec)
    {
        short** mat;
        size_t i, j;

        mat = new short*[rec.size() + 1];
        for (i = 0; i <= rec.size(); i++)
            mat[i] = new short[ref.size() + 1];

        for (i = 0; i <= rec.size(); i++)
            mat[i][0] = short(i);
        for (j = 1; j <= ref.size(); j++)
            mat[0][j] = short(j);

        for (i = 1; i <= rec.size(); i++)
            for (j = 1; j <= ref.size(); j++)
            {
                mat[i][j] = mat[i - 1][j - 1];

                if (rec[i - 1] != ref[j - 1])
                {

                    if ((mat[i - 1][j]) < mat[i][j])
                        mat[i][j] = mat[i - 1][j];
                    if ((mat[i][j - 1]) < mat[i][j])
                        mat[i][j] = mat[i][j - 1];
                    mat[i][j]++;
                }
            }
        float wer = float(mat[rec.size()][ref.size()]) / ref.size();
        for (i = 0; i <= rec.size(); i++)
            delete[] mat[i];
        delete[] mat;
        return wer;
    }
    */

    /*
   void RNNT_decode_oneutt_MBR(std::ref(cn), std::ref(vocabSize), std::ref(blankId), std::ref(deviceid), std::ref(uttFrameNum[uttID]),
                                std::ref(decodeOutputNodeNames), std::ref(decodeInputNodeNames), 
                                std::ref(uttFrameBeginIdx[uttID]), std::ref(uttFrameToChanInd[uttID]), std::ref(numParallelSequences),
                                std::ref(SVD), std::ref(encondeOutput), std::ref(outputNodeNames),
                                std::ref(numBestMBR),  std::ref(lengthNorm),
                                std::ref(wordSeqs[uttID]), std::ref(uttPathsInfo[uttID]), std::ref(vt_onebest_wer[uttID])
    */
    void RNNT_decode_oneutt_MBR(const ComputationNetworkPtr& net, const size_t& vocabSize, const size_t& blankId, const size_t& deviceid, const size_t& uttFrameNum,

                                const std::vector<std::wstring>& decodeOutputNodeNames,
                                const std::vector<std::wstring>& decodeInputNodeNames,
                                const size_t& uttFrameBeginIdx, const size_t& uttFrameToChanInd, const size_t& numParallelSequences,
                                const bool& SVD, const Matrix<ElemType>& encodeOutput, const std::vector<std::wstring>& outputNodeNames,
                                const size_t& numBestMBR, const bool& lengthNorm, const vector<string>& vt_labels,
                                const std::vector<string>& wordSeq, vector<PathInfo>& oneuttPathsInfo, float& onebest_wer,
                                const Matrix<ElemType>& Wm, const Matrix<ElemType>& Wmu, const Matrix<ElemType>& Wmv, const Matrix<ElemType>& bm, const size_t uttID,
                                const ComputationNetwork& decode_net_seed)
    {
        vector<Sequence> CurSequences, nextSequences;
        ComputationNetwork decode_net;
        unordered_map<wstring, vector<shared_ptr<PastValueNode<ElemType>>>> m_nameToPastValueNodeCachePerThread;
        WERFunctions werfs;
        time_t my_time;
        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_oneutt_MBR time 1 = %s, uttFrameNum = %d, uttID = %d \n", ctime(&my_time), int(uttFrameNum), int(uttID));

        vector<shared_ptr<Matrix<ElemType>>> m_decodeOutputCachePerThread;
        // the reason we don't copy from *net, rahter to copy from decode_net_seed is for save GPU memory. As the current decoder part in the *net is much larger
        decode_net.CopySubTree(decode_net_seed, decodeOutputNodeNames[0], L"", CopyNodeFlags::copyNodeAll);
        // m_pMBLayout->CopyFrom(m_minibatchBuffer[index].pMBLayout);
        decode_net.CompileNetwork();
        std::vector<ComputationNodeBasePtr> decodeOutputNodes = decode_net.OutputNodesByName(decodeOutputNodeNames);

        decode_net.FormEvalOrder(decodeOutputNodes[0]);
        decode_net.FormNestedNetwork(decodeOutputNodes[0]);

        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_oneutt_MBR time 2 = %s, uttFrameNum = %d, uttID = %d \n", ctime(&my_time), int(uttFrameNum), int(uttID));

        for (const auto& node : decode_net.GetAllNodesForRoot(decodeOutputNodes[0]))
        {
            if (node->OperationName().find(L"ReduceElements") != string::npos)
            {
                auto rNode = node->As<ReduceElementsNode<ElemType>>();
                rNode->is_multi_thread(true);
            }
        }

        std::vector<ComputationNodeBasePtr> decodeinputNodes = decode_net.OutputNodesByName(decodeInputNodeNames);
        StreamMinibatchInputs decodeinputMatrices = DataReaderHelpersFunctions::RetrieveInputMatrices(decodeinputNodes);

        nextSequences.clear();
        //initialize with blank ID
        Sequence oneSeq = newSeq(vocabSize, (size_t) 50, deviceid);
        extendSeq(oneSeq, blankId, 0.0);

        nextSequences.push_back(oneSeq);

        Matrix<ElemType> decodeOutput(deviceid);

        std::vector<ComputationNodeBasePtr> Plusnodes, Plustransnodes; // as a placeholder, will not be used in multithread case

        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_oneutt_MBR time 3 = %s, uttFrameNum = %d, uttID = %d \n", ctime(&my_time), int(uttFrameNum), int(uttID));

        // loop for each frame
        for (size_t t = 0; t < uttFrameNum; t++)
        {
            //fprintf(stderr, "one utt, uttframenum = %d, t = %d, 1 \n", int(uttFrameNum), int(t));
            for (size_t n = 0; n < CurSequences.size(); n++)
            {
                deleteSeq(CurSequences[n], m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
            }
            vector<Sequence>().swap(CurSequences);
            CurSequences = nextSequences;

            vector<Sequence>().swap(nextSequences);
            //fprintf(stderr,"one utt, uttframenum = %d, t = %d, 2 \n", int(uttFrameNum), int(t));
            int count = 0;
            while (true)
            {

                auto maxSeq = std::max_element(CurSequences.begin(), CurSequences.end());
                Sequence tempSeq = newSeq(*maxSeq, deviceid, m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);

                deleteSeq(*maxSeq, m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
                CurSequences.erase(maxSeq);
                //fprintf(stderr, "while, uttframenum = %d, t = %d, 2 \n", int(uttFrameNum), int(t));
                prepareSequence(tempSeq);

                // mask for debug purpose
                forward_decode(tempSeq, decodeinputMatrices, deviceid, decodeOutputNodes, decodeinputNodes, vocabSize, tempSeq.labelseq.size(), decode_net, int(uttFrameNum));

                size_t tinMB = (t + uttFrameBeginIdx) * numParallelSequences + uttFrameToChanInd;
                //fprintf(stderr, "while, uttframenum = %d, t = %d, 3 \n", int(uttFrameNum), int(t));
                if (SVD)
                    forwardmergedSVD(tempSeq, tinMB, encodeOutput, decodeOutput, Plusnodes, Plustransnodes, Wmu, Wmv, bm, NULL);
                else
                    forwardmerged(tempSeq, tinMB, encodeOutput, decodeOutput, Plusnodes, Plustransnodes, Wm, bm, NULL, int(uttFrameNum), deviceid);
                //sort log posterior and get best N labels
                vector<pair<size_t, ElemType>> topN = getTopN(decodeOutput, numBestMBR, blankId);
                //fprintf(stderr, "while, uttframenum = %d, t = %d, 5 \n", int(uttFrameNum), int(t));
                //expand blank
                Sequence seqK = newSeq(tempSeq, deviceid, m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);

                ElemType newlogP = topN[vocabSize].second + tempSeq.logP;

                seqK.logP = newlogP;
                bool existseq = false;

                for (auto itseq = nextSequences.begin(); itseq != nextSequences.end(); itseq++)
                {
                    //merge the score with same sequence
                    if (seqK.labelseq == itseq->labelseq)
                    {
                        existseq = true;
                        itseq->logP = decodeOutput.LogAdd(seqK.logP, itseq->logP);
                        break;
                    }
                }
                //fprintf(stderr, "while, uttframenum = %d, t = %d, 7 \n", int(uttFrameNum), int(t));
                if (!existseq)
                {
                    nextSequences.push_back(seqK);
                }
                int iLabel;
                for (iLabel = 0; iLabel < numBestMBR; iLabel++)
                {

                    seqK = newSeq(tempSeq, deviceid, m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
                    newlogP = topN[iLabel].second + tempSeq.logP;
                    seqK.logP = newlogP;

                    if (topN[iLabel].first != blankId)

                    {
                        extendSeq(seqK, topN[iLabel].first, newlogP);

                        CurSequences.push_back(seqK);
                    }
                }
                vector<pair<size_t, ElemType>>().swap(topN);
                deleteSeq(tempSeq, m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);

                if (CurSequences.size() == 0)
                    break;
                auto ya = std::max_element(CurSequences.begin(), CurSequences.end());
                auto yb = std::max_element(nextSequences.begin(), nextSequences.end());
                if (nextSequences.size() > numBestMBR && yb->logP > ya->logP)
                    break;
                //fprintf(stderr, "while, uttframenum = %d, t = %d, 11 \n", int(uttFrameNum), int(t));
                count++;
            }
            //fprintf(stderr,"one utt, uttframenum = %d, t = %d, 3 \n", int(uttFrameNum), int(t));
            std::sort(nextSequences.begin(), nextSequences.end());
            std::reverse(nextSequences.begin(), nextSequences.end());
            if (nextSequences.size() > numBestMBR)
            {
                for (size_t n = numBestMBR; n < nextSequences.size(); n++)
                {
                    deleteSeq(nextSequences[n], m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
                }
            }
            for (size_t iseq = nextSequences.size(); iseq > numBestMBR; iseq--)
                nextSequences.pop_back();
        }
        //fprintf(stderr, "one utt, uttframenum = %d, 6 \n", int(uttFrameNum));
        //nbest output
        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_oneutt_MBR time 4 = %s, uttFrameNum = %d, uttID = %d \n", ctime(&my_time), int(uttFrameNum), int(uttID));

        if (nextSequences.size() != 0)
        {
            float totalProb = 0;

            ElemType onebest_lnLogP = ElemType(nextSequences[0].logP / nextSequences[0].labelseq.size());
            size_t onebest_index = 0;

            ElemType lnLogP;
            for (size_t n = 0; n < nextSequences.size(); n++)
            {
                if (n == 0)
                {
                    lnLogP = onebest_lnLogP;
                }
                else
                {
                    lnLogP = ElemType(nextSequences[n].logP / nextSequences[n].labelseq.size());
                    if (lnLogP > onebest_lnLogP)
                    {
                        onebest_lnLogP = lnLogP;
                        onebest_index = n;
                    }
                }

                if (lengthNorm)
                    nextSequences[n].logP = lnLogP;

                nextSequences[n].logP = exp(nextSequences[n].logP); // the logP actually becomes P
                totalProb += float(nextSequences[n].logP);
            }

            for (size_t n = 0; n < nextSequences.size(); n++)
            {
                PathInfo pi;
                pi.prob = float(nextSequences[n].logP / totalProb);

                string word_sequence = "";
                for (size_t k = 0; k < nextSequences[n].length - 1; k++)
                {
                    size_t labelID = nextSequences[n].labelseq[k + 1];
                    if (labelID != (vt_labels.size() - 1)) // it is not <blank>
                    {

                        string wordpiece = vt_labels[labelID];
                        word_sequence += wordpiece;
                    }
                }

                vector<string> vt_words;
                werfs.convert_word_sequence_string_2_vector(word_sequence, vt_words, '_');

                pi.WER = werfs.compute_wer(wordSeq, vt_words);

                pi.label_seq = nextSequences[n].labelseq;

                oneuttPathsInfo.push_back(pi);
            }
            onebest_wer = oneuttPathsInfo[onebest_index].WER;
        }

        for (size_t n = 0; n < CurSequences.size(); n++)
        {
            deleteSeq(CurSequences[n], m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
        }
        //fprintf(stderr, "one utt, uttframenum = %d, 8 \n", int(uttFrameNum));

        vector<Sequence>().swap(CurSequences);
        for (size_t n = 0; n < nextSequences.size(); n++)
        {
            deleteSeq(nextSequences[n], m_nameToPastValueNodeCachePerThread, m_decodeOutputCachePerThread);
        }
        //fprintf(stderr, "one utt, uttframenum = %d, 9 \n", int(uttFrameNum));

        vector<Sequence>().swap(nextSequences);
        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_oneutt_MBR time 5 = %s, uttFrameNum = %d, uttID = %d \n", ctime(&my_time), int(uttFrameNum), int(uttID));

        // end here
    }

    void common_preparations_single_multithread(const std::vector<std::wstring>& outputNodeNames, const ComputationNetworkPtr& net, bool SVD,
                                                Matrix<ElemType>& decodeInputMatrix, MBLayoutPtr& encodeMBLayout, const size_t& deviceid,
                                                MBLayoutPtr& decodeMBLayout, const vector<string>& vt_labels,
                                                size_t& vocabSize, size_t& blankId, std::vector<size_t>& uttFrameNum,
                                                std::vector<std::wstring>& decodeOutputNodeNames, std::vector<ComputationNodeBasePtr>& decodeOutputNodes,
                                                std::vector<size_t>& uttFrameBeginIdx, std::vector<size_t>& uttFrameToChanInd,
                                                size_t& numParallelSequences, size_t& numSequences,
                                                Matrix<ElemType>& Wm, Matrix<ElemType>& Wmu, Matrix<ElemType>& Wmv, Matrix<ElemType>& bm,
                                                MBLayoutPtr& decodebackupMBlayout, Matrix<ElemType>& decodeInputMatrixBackup)
    {
        if (outputNodeNames.size() == 0)
            fprintf(stderr, "OutputNodeNames are not specified, using the default outputnodes.\n");

        //prediction related nodes
        decodeOutputNodeNames.assign(outputNodeNames.begin() + 1, outputNodeNames.begin() + 2);
        decodeOutputNodes = net->OutputNodesByName(decodeOutputNodeNames);

        std::list<ComputationNodeBasePtr> pastValueNodes = net->PastValueNodesForOutputs(decodeOutputNodes);

        std::list<ComputationNodeBasePtr>::iterator it;
        for (it = pastValueNodes.begin(); it != pastValueNodes.end(); ++it)
        {
            auto pastValueNode = dynamic_pointer_cast<PastValueNode<ElemType>>(*it); //DelayedValueNodeBase
            if (pastValueNode || !(*it)->NodeName().compare(0, 5, L"Loop_"))
            {
                m_nodesToCache.push_back((*it)->NodeName());
            }
        }
        //joint nodes
        ComputationNodeBasePtr WmNode, WmuNode, WmvNode, bmNode;
        WmNode;
        WmuNode;
        WmvNode;
        if (SVD)
        {
            WmuNode = net->GetNodeFromName(outputNodeNames[4]);
            WmvNode = net->GetNodeFromName(outputNodeNames[5]);
            bmNode = net->GetNodeFromName(outputNodeNames[6]);
        }
        else
        {
            WmNode = net->GetNodeFromName(outputNodeNames[4]);
            bmNode = net->GetNodeFromName(outputNodeNames[5]);
        }

        std::map<std::wstring, void*, nocase_compare> outputMatrices;

        Matrix<ElemType> maxIdx(deviceid), maxVal(deviceid);

        if (SVD)
        {
            Wmu.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(WmuNode)->Value()));
            Wmv.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(WmvNode)->Value()));
        }
        else
            Wm.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(WmNode)->Value()));

        bm.SetValue(*(&dynamic_pointer_cast<ComputationNode<ElemType>>(bmNode)->Value()));
        const size_t numIterationsBeforePrintingProgress = 100;

        //get MBlayer of encoder input
        numParallelSequences = encodeMBLayout->GetNumParallelSequences();
        numSequences = encodeMBLayout->GetNumSequences();

        uttFrameNum.clear();
        uttFrameToChanInd.clear();
        uttFrameBeginIdx.clear();

        uttFrameNum.reserve(numSequences);
        uttFrameToChanInd.reserve(numSequences);
        uttFrameBeginIdx.reserve(numSequences);

        //get utt information, such as channel map id and utt begin frame, utt frame num, utt phone num for frame and phone respectively....
        size_t seqId = 0; //frame
        size_t totalframenum = 0;

        // this->FormEvalOrder(Plustransnodes[0]);

        for (const auto& seq : encodeMBLayout->GetAllSequences())
        {
            if (seq.seqId == GAP_SEQUENCE_ID)
            {
                continue;
            }
            assert(seq.seqId == seqId);
            seqId++;
            uttFrameToChanInd.push_back(seq.s);
            size_t numFrames = seq.GetNumTimeSteps();
            uttFrameBeginIdx.push_back(seq.tBegin);
            uttFrameNum.push_back(numFrames);
            totalframenum += numFrames;
        }

        //get phone sequene
        CNTK::Matrix<ElemType> maxIndex(deviceid), maxValue(deviceid);
        decodeInputMatrix.VectorMax(maxIndex, maxValue, true);
        maxIndex.TransferToDeviceIfNotThere(CPUDEVICE);

        //backup decoding input matrix and MBlayout

        decodebackupMBlayout = make_shared<MBLayout>();
        decodebackupMBlayout->CopyFrom(decodeMBLayout);

        decodeInputMatrixBackup.SetValue(decodeInputMatrix);

        // the data structure for phone sequence

        // do decoding for the utterances, and feed in the data structure,

        vocabSize = bm.GetNumRows();
        blankId = vocabSize - 1;
        vector<Sequence> CurSequences, nextSequences;
        // sanity check
        if (vt_labels.size() != vocabSize)
        {
            RuntimeError("RNNT_decode_nbest_MBR(_Multithread): size not match, vt_labels.size() = %d, and vocabSize = %d.", int(vt_labels.size()), int(vocabSize));
        }

        // this->FormEvalOrder(Plustransnodes[0]);
    }

    void RNNT_decode_nbest_MBR_Multithread(const std::vector<std::wstring>& outputNodeNames, Matrix<ElemType>& encodeOutput, MBLayoutPtr& encodeMBLayout,
                                           Matrix<ElemType>& decodeInputMatrix, MBLayoutPtr& decodeMBLayout, const std::vector<std::wstring> decodeInputNodeNames,
                                           size_t numBestMBR, bool lengthNorm, const vector<string>& vt_labels, vector<vector<PathInfo>>& uttPathsInfo,
                                           const std::vector<std::vector<string>>& wordSeqs,
                                           vector<float>& vt_onebest_wer, bool SVD, const ComputationNetworkPtr& net, 
                                           const ComputationNetwork& decode_net_seed) /*, size_t num_utt, size_t start_utt) */
    {

        time_t my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR_Multithread time 1 = %s \n", ctime(&my_time));
        size_t vocabSize, blankId, deviceid;
        std::vector<size_t> uttFrameNum;
        std::vector<std::wstring> decodeOutputNodeNames;
        std::vector<size_t> uttFrameBeginIdx;
        std::vector<size_t> uttFrameToChanInd;
        size_t numParallelSequences, numSequences;
        std::vector<ComputationNodeBasePtr> decodeOutputNodes;
        MBLayoutPtr decodebackupMBlayout;
        deviceid = decodeInputMatrix.GetDeviceId();

        Matrix<ElemType> Wm(deviceid), Wmu(deviceid), Wmv(deviceid), bm(deviceid), decodeInputMatrixBackup(deviceid);

        common_preparations_single_multithread(outputNodeNames, net, SVD, decodeInputMatrix, encodeMBLayout, deviceid, decodeMBLayout, vt_labels,
                                               vocabSize, blankId, uttFrameNum, decodeOutputNodeNames, decodeOutputNodes, uttFrameBeginIdx,
                                               uttFrameToChanInd, numParallelSequences, numSequences, Wm, Wmu, Wmv, bm,
                                               decodebackupMBlayout, decodeInputMatrixBackup);

        // this->FormEvalOrder(Plustransnodes[0]);

        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR_Multithread time 2 = %s, numSequences = %d, uttFrameNum = %d \n ", ctime(&my_time), int(numSequences), int(uttFrameNum[0]));
        std::vector<std::thread> vt_threads(numSequences);
        for (size_t uttID = 0; uttID < numSequences; uttID++)
        //for (size_t uttID = start_utt; uttID < num_utt; uttID++)
        {
            vt_threads[uttID] = std::thread(&RNNTDecodeFunctions::RNNT_decode_oneutt_MBR, this, std::ref(net), std::ref(vocabSize), std::ref(blankId),
                                            std::ref(deviceid), std::ref(uttFrameNum[uttID]),
                                            std::ref(decodeOutputNodeNames), std::ref(decodeInputNodeNames),
                                            std::ref(uttFrameBeginIdx[uttID]), std::ref(uttFrameToChanInd[uttID]), std::ref(numParallelSequences),
                                            std::ref(SVD), std::ref(encodeOutput), std::ref(outputNodeNames),
                                            std::ref(numBestMBR), std::ref(lengthNorm), std::ref(vt_labels),
                                            std::ref(wordSeqs[uttID]), std::ref(uttPathsInfo[uttID]), std::ref(vt_onebest_wer[uttID]),
                                            std::ref(Wm), std::ref(Wmu), std::ref(Wmv), std::ref(bm), (uttID),
                                            std::ref(decode_net_seed));

        } // end of for loop

        for (size_t uttID = 0; uttID < numSequences; uttID++)
        // for (size_t uttID = start_utt; uttID < num_utt; uttID++)
        {
            vt_threads[uttID].join();
        }
        decodeInputMatrix.SetValue(decodeInputMatrixBackup);
        //decodeInputMatrix.Print("after ss");
        decodeMBLayout->CopyFrom(decodebackupMBlayout);
        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR_Multithread time 3 = %s \n", ctime(&my_time));
    }

    void RNNT_decode_nbest_MBR(const std::vector<std::wstring>& outputNodeNames, Matrix<ElemType>& encodeOutput, MBLayoutPtr& encodeMBLayout,
                               Matrix<ElemType>& decodeInputMatrix, MBLayoutPtr& decodeMBLayout, std::vector<ComputationNodeBasePtr> decodeinputNodes,
                               size_t numBestMBR, bool lengthNorm, const vector<string>& vt_labels, vector<vector<PathInfo>>& uttPathsInfo,
                               const std::vector<std::vector<string>>& wordSeqs, vector<float>& vt_onebest_wer,
                               bool SVD, const ComputationNetworkPtr& net)
    {
        time_t my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR time 1 = %s \n", ctime(&my_time));

        size_t vocabSize, blankId, deviceid;
        std::vector<size_t> uttFrameNum;
        std::vector<std::wstring> decodeOutputNodeNames;
        std::vector<size_t> uttFrameBeginIdx;
        std::vector<size_t> uttFrameToChanInd;
        size_t numParallelSequences, numSequences;
        std::vector<ComputationNodeBasePtr> decodeOutputNodes;
        MBLayoutPtr decodebackupMBlayout;
        deviceid = decodeInputMatrix.GetDeviceId();

        Matrix<ElemType> Wm(deviceid), Wmu(deviceid), Wmv(deviceid), bm(deviceid), decodeInputMatrixBackup(deviceid);
        WERFunctions werfs;

        common_preparations_single_multithread(outputNodeNames, net, SVD, decodeInputMatrix, encodeMBLayout, deviceid, decodeMBLayout, vt_labels,
                                               vocabSize, blankId, uttFrameNum, decodeOutputNodeNames, decodeOutputNodes,
                                               uttFrameBeginIdx, uttFrameToChanInd, numParallelSequences, numSequences, Wm, Wmu, Wmv, bm,
                                               decodebackupMBlayout, decodeInputMatrixBackup);

        Matrix<ElemType> decodeOutput(deviceid);
        vector<Sequence> CurSequences, nextSequences;
        StreamMinibatchInputs decodeinputMatrices = DataReaderHelpersFunctions::RetrieveInputMatrices(decodeinputNodes);

        std::vector<ComputationNodeBasePtr> Plusnodes, Plustransnodes;
        Plusnodes.push_back(net->GetNodeFromName(outputNodeNames[2]));
        Plustransnodes.push_back(net->GetNodeFromName(outputNodeNames[3]));

        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR time 2 = %s, num_sequence = %d \n", ctime(&my_time), int(numSequences));

        for (size_t uttID = 0; uttID < numSequences; uttID++)
        {
            // fprintf(stderr, "decode v0 uttID = %d .\n", int(uttID));
            nextSequences.clear();
            //initialize with blank ID
            Sequence oneSeq = newSeq(vocabSize, (size_t) 50, deviceid);
            extendSeq(oneSeq, blankId, 0.0);

            nextSequences.push_back(oneSeq);

            // loop for each frame
            for (size_t t = 0; t < uttFrameNum[uttID]; t++)
            {
                for (size_t n = 0; n < CurSequences.size(); n++)
                {
                    deleteSeq(CurSequences[n]);
                }
                vector<Sequence>().swap(CurSequences);
                CurSequences = nextSequences;

                vector<Sequence>().swap(nextSequences);
                //fprintf(stderr, "t = %d .\n", int(t));

                //deal with the same prefix
                //int count = 0;
                while (true)
                {
                    // fprintf(stderr, "count = %d .\n", int(count++));

                    auto maxSeq = std::max_element(CurSequences.begin(), CurSequences.end());
                    Sequence tempSeq = newSeq(*maxSeq, deviceid);
                    deleteSeq(*maxSeq);
                    CurSequences.erase(maxSeq);
                    prepareSequence(tempSeq);
                    forward_decode(tempSeq, decodeinputMatrices, deviceid, decodeOutputNodes, decodeinputNodes, vocabSize, tempSeq.labelseq.size(), *net);

                    size_t tinMB = (t + uttFrameBeginIdx[uttID]) * numParallelSequences + uttFrameToChanInd[uttID];
                    if (SVD)
                        forwardmergedSVD(tempSeq, tinMB, encodeOutput, decodeOutput, Plusnodes, Plustransnodes, Wmu, Wmv, bm, net);
                    else
                        forwardmerged(tempSeq, tinMB, encodeOutput, decodeOutput, Plusnodes, Plustransnodes, Wm, bm, net);

                    //sort log posterior and get best N labels
                    vector<pair<size_t, ElemType>> topN = getTopN(decodeOutput, numBestMBR, blankId);

                    //expand blank
                    Sequence seqK = newSeq(tempSeq, deviceid);
                    ElemType newlogP = topN[vocabSize].second + tempSeq.logP;
                    seqK.logP = newlogP;
                    bool existseq = false;
                    for (auto itseq = nextSequences.begin(); itseq != nextSequences.end(); itseq++)
                    {
                        //merge the score with same sequence
                        if (seqK.labelseq == itseq->labelseq)
                        {
                            existseq = true;
                            itseq->logP = decodeOutput.LogAdd(seqK.logP, itseq->logP);
                            break;
                        }
                    }
                    if (!existseq)
                    {
                        nextSequences.push_back(seqK);
                    }
                    int iLabel;
                    for (iLabel = 0; iLabel < numBestMBR; iLabel++)
                    {

                        seqK = newSeq(tempSeq, deviceid);
                        newlogP = topN[iLabel].second + tempSeq.logP;
                        seqK.logP = newlogP;

                        if (topN[iLabel].first != blankId)

                        {
                            extendSeq(seqK, topN[iLabel].first, newlogP);

                            CurSequences.push_back(seqK);
                        }
                    }
                    vector<pair<size_t, ElemType>>().swap(topN);
                    deleteSeq(tempSeq);

                    if (CurSequences.size() == 0)
                        break;
                    auto ya = std::max_element(CurSequences.begin(), CurSequences.end());
                    auto yb = std::max_element(nextSequences.begin(), nextSequences.end());
                    if (nextSequences.size() > numBestMBR && yb->logP > ya->logP)
                        break;
                }
                std::sort(nextSequences.begin(), nextSequences.end());
                std::reverse(nextSequences.begin(), nextSequences.end());
                if (nextSequences.size() > numBestMBR)
                {
                    for (size_t n = numBestMBR; n < nextSequences.size(); n++)
                    {
                        deleteSeq(nextSequences[n]);
                    }
                }
                for (size_t iseq = nextSequences.size(); iseq > numBestMBR; iseq--)
                    nextSequences.pop_back();
            }

            //nbest output

            if (nextSequences.size() != 0)
            {
                float totalProb = 0;

                ElemType onebest_lnLogP = ElemType(nextSequences[0].logP / nextSequences[0].labelseq.size());
                size_t onebest_index = 0;

                ElemType lnLogP;
                for (size_t n = 0; n < nextSequences.size(); n++)
                {
                    if (n == 0)
                    {
                        lnLogP = onebest_lnLogP;
                    }
                    else
                    {
                        lnLogP = ElemType(nextSequences[n].logP / nextSequences[n].labelseq.size());
                        if (lnLogP > onebest_lnLogP)
                        {
                            onebest_lnLogP = lnLogP;
                            onebest_index = n;
                        }
                    }

                    if (lengthNorm)
                        nextSequences[n].logP = lnLogP;

                    nextSequences[n].logP = exp(nextSequences[n].logP); // the logP actually becomes P
                    totalProb += float(nextSequences[n].logP);
                }

                for (size_t n = 0; n < nextSequences.size(); n++)
                {
                    PathInfo pi;
                    pi.prob = float(nextSequences[n].logP / totalProb);

                    string word_sequence = "";
                    for (size_t k = 0; k < nextSequences[n].length - 1; k++)
                    {
                        size_t labelID = nextSequences[n].labelseq[k + 1];
                        if (labelID != (vt_labels.size() - 1)) // it is not <blank>
                        {

                            string wordpiece = vt_labels[labelID];
                            word_sequence += wordpiece;
                        }
                    }

                    vector<string> vt_words;
                    werfs.convert_word_sequence_string_2_vector(word_sequence, vt_words, '_');

                    pi.WER = werfs.compute_wer(wordSeqs[uttID], vt_words);

                    pi.label_seq = nextSequences[n].labelseq;

                    uttPathsInfo[uttID].push_back(pi);
                }
                vt_onebest_wer[uttID] = uttPathsInfo[uttID][onebest_index].WER;
            }
            for (size_t n = 0; n < CurSequences.size(); n++)
            {
                deleteSeq(CurSequences[n]);
            }
            vector<Sequence>().swap(CurSequences);
            for (size_t n = 0; n < nextSequences.size(); n++)
            {
                deleteSeq(nextSequences[n]);
            }
            vector<Sequence>().swap(nextSequences);
            // end here
            my_time = time(NULL);

            fprintf(stderr, "RNNT_decode_nbest_MBR time 3 = %s, uttID = %d \n", ctime(&my_time), int(uttID));

        } // end of for loop
        decodeInputMatrix.SetValue(decodeInputMatrixBackup);
        //decodeInputMatrix.Print("after ss");
        decodeMBLayout->CopyFrom(decodebackupMBlayout);
        my_time = time(NULL);
        fprintf(stderr, "RNNT_decode_nbest_MBR time 4 = %s \n", ctime(&my_time));
    }
};

// helper that returns 'float' or 'double' depending on ElemType
template <typename ElemType>
static inline const wchar_t* ElemTypeName();
template <>
/*static*/
inline const wchar_t* ElemTypeName<float>()
{
    return L"float";
}
template <>
/*static*/ inline const wchar_t* ElemTypeName<double>()
{
    return L"double";
}
template <>
/*static*/ inline const wchar_t* ElemTypeName<half>()
{
    return L"half";
}

// The following emits the class and enables the BaseMatrix<double> to be available (used by EvalDll)
// The corresponding Matrix<float> is emitted in the SetDeviceId function above.
template class Matrix<double>;
template class Matrix<half>;

// TODOs:
//  - automatic inference of time window w.r.t. delay nodes (and related nodes such as a temporal pooling)
//  - have overrides of RuntimeError etc. in ComputationNode, which prepend the error string with the node name and operation

} // namespace CNTK
} // namespace MSR
} // namespace Microsoft
