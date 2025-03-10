/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 7 End-User License
   Agreement and JUCE Privacy Policy.

   End User License Agreement: www.juce.com/juce-7-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

// Implementation notes:
// On macOS, calling AudioUnitInitialize will internally call AudioObjectGetPropertyData, which
// takes a mutex.
// This same mutex is taken on the audio thread, before calling the audio device's IO callback.
// This is a property of the CoreAudio implementation - we can't remove or interact directly
// with these locks in JUCE.
//
// AudioProcessor instances expect that their callback lock will be taken before calling
// processBlock or processBlockBypassed.
// This means that, to avoid deadlocks, we *always* need to make sure that the CoreAudio mutex
// is locked before taking the callback lock.
// Given that we can't interact with the CoreAudio mutex directly, on the main thread we can't
// call any function that might internally interact with CoreAudio while the callback lock is
// taken.
// In particular, be careful not to call `prepareToPlay` on a hosted AudioUnit from the main
// thread while the callback lock is taken.
// The graph implementation currently makes sure to call prepareToPlay on the main thread,
// without taking the graph's callback lock.

namespace juce
{

/*  Provides a comparison function for various types that have an associated NodeID,
    for use with equal_range, lower_bound etc.
*/
class ImplicitNode
{
public:
    using Node           = AudioProcessorGraph::Node;
    using NodeID         = AudioProcessorGraph::NodeID;
    using NodeAndChannel = AudioProcessorGraph::NodeAndChannel;

    ImplicitNode (NodeID x) : node (x) {}
    ImplicitNode (NodeAndChannel x) : ImplicitNode (x.nodeID) {}
    ImplicitNode (const Node* x) : ImplicitNode (x->nodeID) {}
    ImplicitNode (const std::pair<const NodeAndChannel, std::set<NodeAndChannel>>& x) : ImplicitNode (x.first) {}

    /*  This is the comparison function. */
    static bool compare (ImplicitNode a, ImplicitNode b) { return a.node < b.node; }

private:
    NodeID node;
};

//==============================================================================
/*  A copyable type holding all the nodes, and allowing fast lookup by id. */
class Nodes
{
public:
    using Node           = AudioProcessorGraph::Node;
    using NodeID         = AudioProcessorGraph::NodeID;

    const ReferenceCountedArray<Node>& getNodes() const { return array; }

    Node::Ptr getNodeForId (NodeID nodeID) const
    {
        const auto iter = std::lower_bound (array.begin(), array.end(), nodeID, ImplicitNode::compare);
        return iter != array.end() && (*iter)->nodeID == nodeID ? *iter : nullptr;
    }

    Node::Ptr addNode (std::unique_ptr<AudioProcessor> newProcessor, const NodeID nodeID)
    {
        if (newProcessor == nullptr)
        {
            // Cannot add a null audio processor!
            jassertfalse;
            return {};
        }

        if (std::any_of (array.begin(),
                         array.end(),
                         [&] (auto* n) { return n->getProcessor() == newProcessor.get(); }))
        {
            // This audio processor has already been added to the graph!
            jassertfalse;
            return {};
        }

        const auto iter = std::lower_bound (array.begin(), array.end(), nodeID, ImplicitNode::compare);

        if (iter != array.end() && (*iter)->nodeID == nodeID)
        {
            // This nodeID has already been used for a node in the graph!
            jassertfalse;
            return {};
        }

        return array.insert ((int) std::distance (array.begin(), iter),
                             new Node { nodeID, std::move (newProcessor) });
    }

    Node::Ptr removeNode (NodeID nodeID)
    {
        const auto iter = std::lower_bound (array.begin(), array.end(), nodeID, ImplicitNode::compare);
        return iter != array.end() && (*iter)->nodeID == nodeID
             ? array.removeAndReturn ((int) std::distance (array.begin(), iter))
             : nullptr;
    }

    bool operator== (const Nodes& other) const { return array == other.array; }
    bool operator!= (const Nodes& other) const { return array != other.array; }

private:
    ReferenceCountedArray<Node> array;
};

//==============================================================================
/*  A value type holding a full set of graph connections. */
class Connections
{
public:
    using Node           = AudioProcessorGraph::Node;
    using NodeID         = AudioProcessorGraph::NodeID;
    using Connection     = AudioProcessorGraph::Connection;
    using NodeAndChannel = AudioProcessorGraph::NodeAndChannel;

    static constexpr auto midiChannelIndex = AudioProcessorGraph::midiChannelIndex;

    bool addConnection (const Nodes& n, const Connection& c)
    {
        if (! canConnect (n, c))
            return false;

        sourcesForDestination[c.destination].insert (c.source);
        jassert (isConnected (c));
        return true;
    }

    bool removeConnection (const Connection& c)
    {
        const auto iter = sourcesForDestination.find (c.destination);
        return iter != sourcesForDestination.cend() && iter->second.erase (c.source) == 1;
    }

    bool removeIllegalConnections (const Nodes& n)
    {
        auto anyRemoved = false;

        for (auto& dest : sourcesForDestination)
        {
            const auto initialSize = dest.second.size();
            dest.second = removeIllegalConnections (n, std::move (dest.second), dest.first);
            anyRemoved |= (dest.second.size() != initialSize);
        }

        return anyRemoved;
    }

    bool disconnectNode (NodeID n)
    {
        const auto matchingDestinations = getMatchingDestinations (n);
        auto result = matchingDestinations.first != matchingDestinations.second;
        sourcesForDestination.erase (matchingDestinations.first, matchingDestinations.second);

        for (auto& pair : sourcesForDestination)
        {
            const auto range = std::equal_range (pair.second.cbegin(), pair.second.cend(), n, ImplicitNode::compare);
            result |= range.first != range.second;
            pair.second.erase (range.first, range.second);
        }

        return result;
    }

    static bool isConnectionLegal (const Nodes& n, Connection c)
    {
        const auto source = n.getNodeForId (c.source     .nodeID);
        const auto dest   = n.getNodeForId (c.destination.nodeID);

        const auto sourceChannel = c.source     .channelIndex;
        const auto destChannel   = c.destination.channelIndex;

        const auto sourceIsMIDI = AudioProcessorGraph::midiChannelIndex == sourceChannel;
        const auto destIsMIDI   = AudioProcessorGraph::midiChannelIndex == destChannel;

        return sourceChannel >= 0
            && destChannel >= 0
            && source != dest
            && sourceIsMIDI == destIsMIDI
            && source != nullptr
            && (sourceIsMIDI
                    ? source->getProcessor()->producesMidi()
                    : sourceChannel < source->getProcessor()->getTotalNumOutputChannels())
            && dest != nullptr
            && (destIsMIDI
                    ? dest->getProcessor()->acceptsMidi()
                    : destChannel < dest->getProcessor()->getTotalNumInputChannels());
    }

    bool canConnect (const Nodes& n, Connection c) const
    {
        return isConnectionLegal (n, c) && ! isConnected (c);
    }

    bool isConnected (Connection c) const
    {
        const auto iter = sourcesForDestination.find (c.destination);

        return iter != sourcesForDestination.cend()
               && iter->second.find (c.source) != iter->second.cend();
    }

    bool isConnected (NodeID srcID, NodeID destID) const
    {
        const auto matchingDestinations = getMatchingDestinations (destID);

        return std::any_of (matchingDestinations.first, matchingDestinations.second, [srcID] (const auto& pair)
        {
            const auto iter = std::lower_bound (pair.second.cbegin(), pair.second.cend(), srcID, ImplicitNode::compare);
            return iter != pair.second.cend() && iter->nodeID == srcID;
        });
    }

    std::set<NodeID> getSourceNodesForDestination (NodeID destID) const
    {
        const auto matchingDestinations = getMatchingDestinations (destID);

        std::set<NodeID> result;
        std::for_each (matchingDestinations.first, matchingDestinations.second, [&] (const auto& pair)
        {
            for (const auto& source : pair.second)
                result.insert (source.nodeID);
        });
        return result;
    }

    std::set<NodeAndChannel> getSourcesForDestination (const NodeAndChannel& p) const
    {
        const auto iter = sourcesForDestination.find (p);
        return iter != sourcesForDestination.cend() ? iter->second : std::set<NodeAndChannel>{};
    }

    std::vector<Connection> getConnections() const
    {
        std::vector<Connection> result;

        for (auto& pair : sourcesForDestination)
            for (const auto& source : pair.second)
                result.emplace_back (source, pair.first);

        std::sort (result.begin(), result.end());
        result.erase (std::unique (result.begin(), result.end()), result.end());
        return result;
    }

    bool isAnInputTo (NodeID source, NodeID dest) const
    {
        return getConnectedRecursive (source, dest, {}).found;
    }

    bool operator== (const Connections& other) const { return sourcesForDestination == other.sourcesForDestination; }
    bool operator!= (const Connections& other) const { return sourcesForDestination != other.sourcesForDestination; }

private:
    using Map = std::map<NodeAndChannel, std::set<NodeAndChannel>>;

    struct SearchState
    {
        std::set<NodeID> visited;
        bool found = false;
    };

    SearchState getConnectedRecursive (NodeID source, NodeID dest, SearchState state) const
    {
        state.visited.insert (dest);

        for (const auto& s : getSourceNodesForDestination (dest))
        {
            if (state.found || s == source)
                return { std::move (state.visited), true };

            if (state.visited.find (s) == state.visited.cend())
                state = getConnectedRecursive (source, s, std::move (state));
        }

        return state;
    }

    static std::set<NodeAndChannel> removeIllegalConnections (const Nodes& nodes,
                                                              std::set<NodeAndChannel> sources,
                                                              NodeAndChannel destination)
    {
        for (auto source = sources.cbegin(); source != sources.cend();)
        {
            if (! isConnectionLegal (nodes, { *source, destination }))
                source = sources.erase (source);
            else
                ++source;
        }

        return sources;
    }

    std::pair<Map::const_iterator, Map::const_iterator> getMatchingDestinations (NodeID destID) const
    {
        return std::equal_range (sourcesForDestination.cbegin(), sourcesForDestination.cend(), destID, ImplicitNode::compare);
    }

    Map sourcesForDestination;
};

//==============================================================================
/*  Settings used to prepare a node for playback. */
struct PrepareSettings
{
    using ProcessingPrecision = AudioProcessorGraph::ProcessingPrecision;

    ProcessingPrecision precision = ProcessingPrecision::singlePrecision;
    double sampleRate             = 0.0;
    int blockSize                 = 0;

    auto tie() const noexcept { return std::tie (precision, sampleRate, blockSize); }

    bool operator== (const PrepareSettings& other) const { return tie() == other.tie(); }
    bool operator!= (const PrepareSettings& other) const { return tie() != other.tie(); }
};

//==============================================================================
/*  Keeps track of the PrepareSettings applied to each node. */
class NodeStates
{
public:
    using Node           = AudioProcessorGraph::Node;
    using NodeID         = AudioProcessorGraph::NodeID;

    /*  Called from prepareToPlay and releaseResources with the PrepareSettings that should be
        used next time the graph is rebuilt.
    */
    void setState (Optional<PrepareSettings> newSettings)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        next = newSettings;
    }

    /*  Call from the audio thread only. */
    Optional<PrepareSettings> getLastRequestedSettings() const { return next; }

    /*  Call from the main thread only!

        Called after updating the graph topology to prepare any currently-unprepared nodes.

        To ensure that all nodes are initialised with the same sample rate, buffer size, etc. as
        the enclosing graph, we must ensure that any operation that uses these details (preparing
        individual nodes) is synchronized with prepare-to-play and release-resources on the
        enclosing graph.

        If the new PrepareSettings are different to the last-seen settings, all nodes will
        be prepared/unprepared as necessary. If the PrepareSettings have not changed, then only
        new nodes will be prepared/unprepared.

        Returns the settings that were applied to the nodes.
    */
    Optional<PrepareSettings> applySettings (const Nodes& n)
    {
        const auto settingsChanged = [this]
        {
            const std::lock_guard<std::mutex> lock (mutex);
            const auto result = current != next;
            current = next;
            return result;
        }();

        // It may look like releaseResources and prepareToPlay could race with calls to processBlock
        // here, because applySettings is called from the main thread, processBlock is called from
        // the audio thread (normally), and there's no explicit mutex ensuring that the calls don't
        // overlap.
        // However, it is part of the AudioProcessor contract that users shall not call
        // processBlock, prepareToPlay, and/or releaseResources concurrently. That is, there's an
        // implied mutex synchronising these functions on each AudioProcessor.
        //
        // Inside processBlock, we always ensure that the current RenderSequence's PrepareSettings
        // match the graph's settings before attempting to call processBlock on any of the graph
        // nodes; as a result, it's impossible to start calling processBlock on a node on the audio
        // thread while a render sequence rebuild (including prepareToPlay/releaseResources calls)
        // is already in progress here.
        //
        // Due to the implied mutex between prepareToPlay/releaseResources/processBlock, it's also
        // impossible to receive new PrepareSettings and to start a new RenderSequence rebuild while
        // a processBlock call is in progress.

        if (settingsChanged)
        {
            for (const auto& node : n.getNodes())
                node->getProcessor()->releaseResources();

            preparedNodes.clear();
        }

        if (current.hasValue())
        {
            for (const auto& node : n.getNodes())
            {
                if (preparedNodes.find (node->nodeID) != preparedNodes.cend())
                    continue;

                preparedNodes.insert (node->nodeID);

                node->getProcessor()->setProcessingPrecision (node->getProcessor()->supportsDoublePrecisionProcessing() ? current->precision
                                                                                                                        : AudioProcessor::singlePrecision);
                node->getProcessor()->setRateAndBufferSizeDetails (current->sampleRate, current->blockSize);
                node->getProcessor()->prepareToPlay               (current->sampleRate, current->blockSize);
            }
        }

        return current;
    }

private:
    std::mutex mutex;
    std::set<NodeID> preparedNodes;
    Optional<PrepareSettings> current, next;
};

//==============================================================================
template <typename FloatType>
struct GraphRenderSequence
{
    using Node = AudioProcessorGraph::Node;

    struct Context
    {
        FloatType* const* audioBuffers;
        MidiBuffer* midiBuffers;
        AudioPlayHead* audioPlayHead;
        int numSamples;
    };

    void perform (AudioBuffer<FloatType>& buffer, MidiBuffer& midiMessages, AudioPlayHead* audioPlayHead)
    {
        auto numSamples = buffer.getNumSamples();
        auto maxSamples = renderingBuffer.getNumSamples();

        if (numSamples > maxSamples)
        {
            // Being asked to render more samples than our buffers have, so divide the buffer into chunks
            int chunkStartSample = 0;
            while (chunkStartSample < numSamples)
            {
                auto chunkSize = jmin (maxSamples, numSamples - chunkStartSample);

                AudioBuffer<FloatType> audioChunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), chunkStartSample, chunkSize);
                midiChunk.clear();
                midiChunk.addEvents (midiMessages, chunkStartSample, chunkSize, -chunkStartSample);

                // Splitting up the buffer like this will cause the play head and host time to be
                // invalid for all but the first chunk...
                perform (audioChunk, midiChunk, audioPlayHead);

                chunkStartSample += maxSamples;
            }

            return;
        }

        currentAudioInputBuffer = &buffer;
        currentAudioOutputBuffer.setSize (jmax (1, buffer.getNumChannels()), numSamples);
        currentAudioOutputBuffer.clear();
        currentMidiInputBuffer = &midiMessages;
        currentMidiOutputBuffer.clear();

        {
            const Context context { renderingBuffer.getArrayOfWritePointers(), midiBuffers.begin(), audioPlayHead, numSamples };

            for (const auto& op : renderOps)
                op (context);
        }

        for (int i = 0; i < buffer.getNumChannels(); ++i)
            buffer.copyFrom (i, 0, currentAudioOutputBuffer, i, 0, numSamples);

        midiMessages.clear();
        midiMessages.addEvents (currentMidiOutputBuffer, 0, buffer.getNumSamples(), 0);
        currentAudioInputBuffer = nullptr;
    }

    JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4661)

    void addClearChannelOp (int index)
    {
        renderOps.push_back ([=] (const Context& c)    { FloatVectorOperations::clear (c.audioBuffers[index], c.numSamples); });
    }

    void addCopyChannelOp (int srcIndex, int dstIndex)
    {
        renderOps.push_back ([=] (const Context& c)    { FloatVectorOperations::copy (c.audioBuffers[dstIndex], c.audioBuffers[srcIndex], c.numSamples); });
    }

    void addAddChannelOp (int srcIndex, int dstIndex)
    {
        renderOps.push_back ([=] (const Context& c)    { FloatVectorOperations::add (c.audioBuffers[dstIndex], c.audioBuffers[srcIndex], c.numSamples); });
    }

    JUCE_END_IGNORE_WARNINGS_MSVC

    void addClearMidiBufferOp (int index)
    {
        renderOps.push_back ([=] (const Context& c)    { c.midiBuffers[index].clear(); });
    }

    void addCopyMidiBufferOp (int srcIndex, int dstIndex)
    {
        renderOps.push_back ([=] (const Context& c)    { c.midiBuffers[dstIndex] = c.midiBuffers[srcIndex]; });
    }

    void addAddMidiBufferOp (int srcIndex, int dstIndex)
    {
        renderOps.push_back ([=] (const Context& c)    { c.midiBuffers[dstIndex].addEvents (c.midiBuffers[srcIndex], 0, c.numSamples, 0); });
    }

    void addDelayChannelOp (int chan, int delaySize)
    {
        renderOps.push_back (DelayChannelOp { chan, delaySize });
    }

    void addProcessOp (const Node::Ptr& node,
                       const Array<int>& audioChannelsUsed,
                       int totalNumChans,
                       int midiBuffer)
    {
        renderOps.push_back (ProcessOp { node, audioChannelsUsed, totalNumChans, midiBuffer });
    }

    void prepareBuffers (int blockSize)
    {
        renderingBuffer.setSize (numBuffersNeeded + 1, blockSize);
        renderingBuffer.clear();
        currentAudioOutputBuffer.setSize (numBuffersNeeded + 1, blockSize);
        currentAudioOutputBuffer.clear();

        currentAudioInputBuffer = nullptr;
        currentMidiInputBuffer = nullptr;
        currentMidiOutputBuffer.clear();

        midiBuffers.clearQuick();
        midiBuffers.resize (numMidiBuffersNeeded);

        const int defaultMIDIBufferSize = 512;

        midiChunk.ensureSize (defaultMIDIBufferSize);

        for (auto&& m : midiBuffers)
            m.ensureSize (defaultMIDIBufferSize);
    }

    void releaseBuffers()
    {
        renderingBuffer.setSize (1, 1);
        currentAudioOutputBuffer.setSize (1, 1);
        currentAudioInputBuffer = nullptr;
        currentMidiInputBuffer = nullptr;
        currentMidiOutputBuffer.clear();
        midiBuffers.clear();
    }

    int numBuffersNeeded = 0, numMidiBuffersNeeded = 0;

    AudioBuffer<FloatType> renderingBuffer, currentAudioOutputBuffer;
    AudioBuffer<FloatType>* currentAudioInputBuffer = nullptr;

    MidiBuffer* currentMidiInputBuffer = nullptr;
    MidiBuffer currentMidiOutputBuffer;

    Array<MidiBuffer> midiBuffers;
    MidiBuffer midiChunk;

private:
    //==============================================================================
    std::vector<std::function<void (const Context&)>> renderOps;

    //==============================================================================
    struct DelayChannelOp
    {
        DelayChannelOp (int chan, int delaySize)
            : buffer ((size_t) (delaySize + 1), (FloatType) 0),
              channel (chan),
              writeIndex (delaySize)
        {
        }

        void operator() (const Context& c)
        {
            auto* data = c.audioBuffers[channel];

            for (int i = c.numSamples; --i >= 0;)
            {
                buffer[(size_t) writeIndex] = *data;
                *data++ = buffer[(size_t) readIndex];

                if (++readIndex  >= (int) buffer.size()) readIndex = 0;
                if (++writeIndex >= (int) buffer.size()) writeIndex = 0;
            }
        }

        std::vector<FloatType> buffer;
        const int channel;
        int readIndex = 0, writeIndex;
    };

    //==============================================================================
    struct ProcessOp
    {
        ProcessOp (const Node::Ptr& n,
                   const Array<int>& audioChannelsUsed,
                   int totalNumChans, int midiBuffer)
            : node (n),
              processor (*n->getProcessor()),
              audioChannelsToUse (audioChannelsUsed),
              audioChannels ((size_t) jmax (1, totalNumChans), nullptr),
              midiBufferToUse (midiBuffer)
        {
            while (audioChannelsToUse.size() < (int) audioChannels.size())
                audioChannelsToUse.add (0);
        }

        void operator() (const Context& c)
        {
            processor.setPlayHead (c.audioPlayHead);

            for (size_t i = 0; i < audioChannels.size(); ++i)
                audioChannels[i] = c.audioBuffers[audioChannelsToUse.getUnchecked ((int) i)];

            auto numAudioChannels = [this]
            {
                if (const auto* proc = node->getProcessor())
                    if (proc->getTotalNumInputChannels() == 0 && proc->getTotalNumOutputChannels() == 0)
                        return 0;

                return (int) audioChannels.size();
            }();

            AudioBuffer<FloatType> buffer { audioChannels.data(), numAudioChannels, c.numSamples };

            const ScopedLock lock (processor.getCallbackLock());

            if (processor.isSuspended())
                buffer.clear();
            else
                callProcess (buffer, c.midiBuffers[midiBufferToUse]);
        }

        void callProcess (AudioBuffer<float>& buffer, MidiBuffer& midi)
        {
            if (processor.isUsingDoublePrecision())
            {
                tempBufferDouble.makeCopyOf (buffer, true);
                process (*node, tempBufferDouble, midi);
                buffer.makeCopyOf (tempBufferDouble, true);
            }
            else
            {
                process (*node, buffer, midi);
            }
        }

        void callProcess (AudioBuffer<double>& buffer, MidiBuffer& midi)
        {
            if (processor.isUsingDoublePrecision())
            {
                process (*node, buffer, midi);
            }
            else
            {
                tempBufferFloat.makeCopyOf (buffer, true);
                process (*node, tempBufferFloat, midi);
                buffer.makeCopyOf (tempBufferFloat, true);
            }
        }

        template <typename Value>
        static void process (const Node& node, AudioBuffer<Value>& audio, MidiBuffer& midi)
        {
            if (node.isBypassed() && node.getProcessor()->getBypassParameter() == nullptr)
                node.getProcessor()->processBlockBypassed (audio, midi);
            else
                node.getProcessor()->processBlock (audio, midi);
        }

        const Node::Ptr node;
        AudioProcessor& processor;

        Array<int> audioChannelsToUse;
        std::vector<FloatType*> audioChannels;
        AudioBuffer<float> tempBufferFloat, tempBufferDouble;
        const int midiBufferToUse;
    };
};

//==============================================================================
class RenderSequenceBuilder
{
public:
    using Node           = AudioProcessorGraph::Node;
    using NodeID         = AudioProcessorGraph::NodeID;
    using Connection     = AudioProcessorGraph::Connection;
    using NodeAndChannel = AudioProcessorGraph::NodeAndChannel;

    static constexpr auto midiChannelIndex = AudioProcessorGraph::midiChannelIndex;

    template <typename RenderSequence>
    static auto build (const Nodes& n, const Connections& c)
    {
        RenderSequence sequence;
        const RenderSequenceBuilder builder (n, c, sequence);

        struct SequenceAndLatency
        {
            RenderSequence sequence;
            int latencySamples = 0;
        };

        return SequenceAndLatency { std::move (sequence), builder.totalLatency };
    }

private:
    //==============================================================================
    const Array<Node*> orderedNodes;

    struct AssignedBuffer
    {
        NodeAndChannel channel;

        static AssignedBuffer createReadOnlyEmpty() noexcept    { return { { zeroNodeID(), 0 } }; }
        static AssignedBuffer createFree() noexcept             { return { { freeNodeID(), 0 } }; }

        bool isReadOnlyEmpty() const noexcept                   { return channel.nodeID == zeroNodeID(); }
        bool isFree() const noexcept                            { return channel.nodeID == freeNodeID(); }
        bool isAssigned() const noexcept                        { return ! (isReadOnlyEmpty() || isFree()); }

        void setFree() noexcept                                 { channel = { freeNodeID(), 0 }; }
        void setAssignedToNonExistentNode() noexcept            { channel = { anonNodeID(), 0 }; }

    private:
        static NodeID anonNodeID() { return NodeID (0x7ffffffd); }
        static NodeID zeroNodeID() { return NodeID (0x7ffffffe); }
        static NodeID freeNodeID() { return NodeID (0x7fffffff); }
    };

    Array<AssignedBuffer> audioBuffers, midiBuffers;

    enum { readOnlyEmptyBufferIndex = 0 };

    HashMap<uint32, int> delays;
    int totalLatency = 0;

    int getNodeDelay (NodeID nodeID) const noexcept
    {
        return delays[nodeID.uid];
    }

    int getInputLatencyForNode (const Connections& c, NodeID nodeID) const
    {
        const auto sources = c.getSourceNodesForDestination (nodeID);
        return std::accumulate (sources.cbegin(), sources.cend(), 0, [this] (auto acc, auto source)
        {
            return jmax (acc, this->getNodeDelay (source));
        });
    }

    //==============================================================================
    void getAllParentsOfNode (const NodeID& child,
                              std::set<NodeID>& parents,
                              const std::map<NodeID, std::set<NodeID>>& otherParents,
                              const Connections& c)
    {
        for (const auto& parentNode : c.getSourceNodesForDestination (child))
        {
            if (parentNode == child)
                continue;

            if (parents.insert (parentNode).second)
            {
                const auto parentParents = otherParents.find (parentNode);

                if (parentParents != otherParents.end())
                {
                    parents.insert (parentParents->second.begin(), parentParents->second.end());
                    continue;
                }

                getAllParentsOfNode (parentNode, parents, otherParents, c);
            }
        }
    }

    Array<Node*> createOrderedNodeList (const Nodes& n, const Connections& c)
    {
        Array<Node*> result;

        std::map<NodeID, std::set<NodeID>> nodeParents;

        for (auto& node : n.getNodes())
        {
            const auto nodeID = node->nodeID;
            int insertionIndex = 0;

            for (; insertionIndex < result.size(); ++insertionIndex)
            {
                auto& parents = nodeParents[result.getUnchecked (insertionIndex)->nodeID];

                if (parents.find (nodeID) != parents.end())
                    break;
            }

            result.insert (insertionIndex, node);
            getAllParentsOfNode (nodeID, nodeParents[node->nodeID], nodeParents, c);
        }

        return result;
    }

    //==============================================================================
    template <typename RenderSequence>
    int findBufferForInputAudioChannel (const Connections& c,
                                        RenderSequence& sequence,
                                        Node& node,
                                        const int inputChan,
                                        const int ourRenderingIndex,
                                        const int maxLatency)
    {
        auto& processor = *node.getProcessor();
        auto numOuts = processor.getTotalNumOutputChannels();

        auto sources = c.getSourcesForDestination ({ node.nodeID, inputChan });

        // Handle an unconnected input channel...
        if (sources.empty())
        {
            if (inputChan >= numOuts)
                return readOnlyEmptyBufferIndex;

            auto index = getFreeBuffer (audioBuffers);
            sequence.addClearChannelOp (index);
            return index;
        }

        // Handle an input from a single source..
        if (sources.size() == 1)
        {
            // channel with a straightforward single input..
            auto src = *sources.begin();

            int bufIndex = getBufferContaining (src);

            if (bufIndex < 0)
            {
                // if not found, this is probably a feedback loop
                bufIndex = readOnlyEmptyBufferIndex;
                jassert (bufIndex >= 0);
            }

            if (inputChan < numOuts && isBufferNeededLater (c, ourRenderingIndex, inputChan, src))
            {
                // can't mess up this channel because it's needed later by another node,
                // so we need to use a copy of it..
                auto newFreeBuffer = getFreeBuffer (audioBuffers);
                sequence.addCopyChannelOp (bufIndex, newFreeBuffer);
                bufIndex = newFreeBuffer;
            }

            auto nodeDelay = getNodeDelay (src.nodeID);

            if (nodeDelay < maxLatency)
                sequence.addDelayChannelOp (bufIndex, maxLatency - nodeDelay);

            return bufIndex;
        }

        // Handle a mix of several outputs coming into this input..
        int reusableInputIndex = -1;
        int bufIndex = -1;

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                auto sourceBufIndex = getBufferContaining (src);

                if (sourceBufIndex >= 0 && ! isBufferNeededLater (c, ourRenderingIndex, inputChan, src))
                {
                    // we've found one of our input chans that can be re-used..
                    reusableInputIndex = i;
                    bufIndex = sourceBufIndex;

                    auto nodeDelay = getNodeDelay (src.nodeID);

                    if (nodeDelay < maxLatency)
                        sequence.addDelayChannelOp (bufIndex, maxLatency - nodeDelay);

                    break;
                }

                ++i;
            }
        }

        if (reusableInputIndex < 0)
        {
            // can't re-use any of our input chans, so get a new one and copy everything into it..
            bufIndex = getFreeBuffer (audioBuffers);
            jassert (bufIndex != 0);

            audioBuffers.getReference (bufIndex).setAssignedToNonExistentNode();

            auto srcIndex = getBufferContaining (*sources.begin());

            if (srcIndex < 0)
                sequence.addClearChannelOp (bufIndex);  // if not found, this is probably a feedback loop
            else
                sequence.addCopyChannelOp (srcIndex, bufIndex);

            reusableInputIndex = 0;
            auto nodeDelay = getNodeDelay (sources.begin()->nodeID);

            if (nodeDelay < maxLatency)
                sequence.addDelayChannelOp (bufIndex, maxLatency - nodeDelay);
        }

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                if (i != reusableInputIndex)
                {
                    int srcIndex = getBufferContaining (src);

                    if (srcIndex >= 0)
                    {
                        auto nodeDelay = getNodeDelay (src.nodeID);

                        if (nodeDelay < maxLatency)
                        {
                            if (! isBufferNeededLater (c, ourRenderingIndex, inputChan, src))
                            {
                                sequence.addDelayChannelOp (srcIndex, maxLatency - nodeDelay);
                            }
                            else // buffer is reused elsewhere, can't be delayed
                            {
                                auto bufferToDelay = getFreeBuffer (audioBuffers);
                                sequence.addCopyChannelOp (srcIndex, bufferToDelay);
                                sequence.addDelayChannelOp (bufferToDelay, maxLatency - nodeDelay);
                                srcIndex = bufferToDelay;
                            }
                        }

                        sequence.addAddChannelOp (srcIndex, bufIndex);
                    }
                }

                ++i;
            }
        }

        return bufIndex;
    }

    template <typename RenderSequence>
    int findBufferForInputMidiChannel (const Connections& c,
                                       RenderSequence& sequence,
                                       Node& node,
                                       int ourRenderingIndex)
    {
        auto& processor = *node.getProcessor();
        auto sources = c.getSourcesForDestination ({ node.nodeID, midiChannelIndex });

        // No midi inputs..
        if (sources.empty())
        {
            auto midiBufferToUse = getFreeBuffer (midiBuffers); // need to pick a buffer even if the processor doesn't use midi

            if (processor.acceptsMidi() || processor.producesMidi())
                sequence.addClearMidiBufferOp (midiBufferToUse);

            return midiBufferToUse;
        }

        // One midi input..
        if (sources.size() == 1)
        {
            auto src = *sources.begin();
            auto midiBufferToUse = getBufferContaining (src);

            if (midiBufferToUse >= 0)
            {
                if (isBufferNeededLater (c, ourRenderingIndex, midiChannelIndex, src))
                {
                    // can't mess up this channel because it's needed later by another node, so we
                    // need to use a copy of it..
                    auto newFreeBuffer = getFreeBuffer (midiBuffers);
                    sequence.addCopyMidiBufferOp (midiBufferToUse, newFreeBuffer);
                    midiBufferToUse = newFreeBuffer;
                }
            }
            else
            {
                // probably a feedback loop, so just use an empty one..
                midiBufferToUse = getFreeBuffer (midiBuffers); // need to pick a buffer even if the processor doesn't use midi
            }

            return midiBufferToUse;
        }

        // Multiple midi inputs..
        int midiBufferToUse = -1;
        int reusableInputIndex = -1;

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                auto sourceBufIndex = getBufferContaining (src);

                if (sourceBufIndex >= 0
                    && ! isBufferNeededLater (c, ourRenderingIndex, midiChannelIndex, src))
                {
                    // we've found one of our input buffers that can be re-used..
                    reusableInputIndex = i;
                    midiBufferToUse = sourceBufIndex;
                    break;
                }

                ++i;
            }
        }

        if (reusableInputIndex < 0)
        {
            // can't re-use any of our input buffers, so get a new one and copy everything into it..
            midiBufferToUse = getFreeBuffer (midiBuffers);
            jassert (midiBufferToUse >= 0);

            auto srcIndex = getBufferContaining (*sources.begin());

            if (srcIndex >= 0)
                sequence.addCopyMidiBufferOp (srcIndex, midiBufferToUse);
            else
                sequence.addClearMidiBufferOp (midiBufferToUse);

            reusableInputIndex = 0;
        }

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                if (i != reusableInputIndex)
                {
                    auto srcIndex = getBufferContaining (src);

                    if (srcIndex >= 0)
                        sequence.addAddMidiBufferOp (srcIndex, midiBufferToUse);
                }

                ++i;
            }
        }

        return midiBufferToUse;
    }

    template <typename RenderSequence>
    void createRenderingOpsForNode (const Connections& c,
                                    RenderSequence& sequence,
                                    Node& node,
                                    const int ourRenderingIndex)
    {
        auto& processor = *node.getProcessor();
        auto numIns  = processor.getTotalNumInputChannels();
        auto numOuts = processor.getTotalNumOutputChannels();
        auto totalChans = jmax (numIns, numOuts);

        Array<int> audioChannelsToUse;
        auto maxLatency = getInputLatencyForNode (c, node.nodeID);

        for (int inputChan = 0; inputChan < numIns; ++inputChan)
        {
            // get a list of all the inputs to this node
            auto index = findBufferForInputAudioChannel (c,
                                                         sequence,
                                                         node,
                                                         inputChan,
                                                         ourRenderingIndex,
                                                         maxLatency);
            jassert (index >= 0);

            audioChannelsToUse.add (index);

            if (inputChan < numOuts)
                audioBuffers.getReference (index).channel = { node.nodeID, inputChan };
        }

        for (int outputChan = numIns; outputChan < numOuts; ++outputChan)
        {
            auto index = getFreeBuffer (audioBuffers);
            jassert (index != 0);
            audioChannelsToUse.add (index);

            audioBuffers.getReference (index).channel = { node.nodeID, outputChan };
        }

        auto midiBufferToUse = findBufferForInputMidiChannel (c, sequence, node, ourRenderingIndex);

        if (processor.producesMidi())
            midiBuffers.getReference (midiBufferToUse).channel = { node.nodeID, midiChannelIndex };

        delays.set (node.nodeID.uid, maxLatency + processor.getLatencySamples());

        if (numOuts == 0)
            totalLatency = maxLatency;

        sequence.addProcessOp (node, audioChannelsToUse, totalChans, midiBufferToUse);
    }

    //==============================================================================
    static int getFreeBuffer (Array<AssignedBuffer>& buffers)
    {
        for (int i = 1; i < buffers.size(); ++i)
            if (buffers.getReference (i).isFree())
                return i;

        buffers.add (AssignedBuffer::createFree());
        return buffers.size() - 1;
    }

    int getBufferContaining (NodeAndChannel output) const noexcept
    {
        int i = 0;

        for (auto& b : output.isMIDI() ? midiBuffers : audioBuffers)
        {
            if (b.channel == output)
                return i;

            ++i;
        }

        return -1;
    }

    void markAnyUnusedBuffersAsFree (const Connections& c,
                                     Array<AssignedBuffer>& buffers,
                                     const int stepIndex)
    {
        for (auto& b : buffers)
            if (b.isAssigned() && ! isBufferNeededLater (c, stepIndex, -1, b.channel))
                b.setFree();
    }

    bool isBufferNeededLater (const Connections& c,
                              int stepIndexToSearchFrom,
                              int inputChannelOfIndexToIgnore,
                              NodeAndChannel output) const
    {
        while (stepIndexToSearchFrom < orderedNodes.size())
        {
            auto* node = orderedNodes.getUnchecked (stepIndexToSearchFrom);

            if (output.isMIDI())
            {
                if (inputChannelOfIndexToIgnore != midiChannelIndex
                    && c.isConnected ({ { output.nodeID, midiChannelIndex },
                                        { node->nodeID,  midiChannelIndex } }))
                    return true;
            }
            else
            {
                for (int i = 0; i < node->getProcessor()->getTotalNumInputChannels(); ++i)
                    if (i != inputChannelOfIndexToIgnore && c.isConnected ({ output, { node->nodeID, i } }))
                        return true;
            }

            inputChannelOfIndexToIgnore = -1;
            ++stepIndexToSearchFrom;
        }

        return false;
    }

    template <typename RenderSequence>
    RenderSequenceBuilder (const Nodes& n, const Connections& c, RenderSequence& sequence)
        : orderedNodes (createOrderedNodeList (n, c))
    {
        audioBuffers.add (AssignedBuffer::createReadOnlyEmpty()); // first buffer is read-only zeros
        midiBuffers .add (AssignedBuffer::createReadOnlyEmpty());

        for (int i = 0; i < orderedNodes.size(); ++i)
        {
            createRenderingOpsForNode (c, sequence, *orderedNodes.getUnchecked (i), i);
            markAnyUnusedBuffersAsFree (c, audioBuffers, i);
            markAnyUnusedBuffersAsFree (c, midiBuffers, i);
        }

        sequence.numBuffersNeeded = audioBuffers.size();
        sequence.numMidiBuffersNeeded = midiBuffers.size();
    }
};

//==============================================================================
/*  A full graph of audio processors, ready to process at a particular sample rate, block size,
    and precision.

    Instances of this class will be created on the main thread, and then passed over to the audio
    thread for processing.
*/
class RenderSequence
{
public:
    using AudioGraphIOProcessor = AudioProcessorGraph::AudioGraphIOProcessor;

    RenderSequence (PrepareSettings s, const Nodes& n, const Connections& c)
        : RenderSequence (s,
                          RenderSequenceBuilder::build<GraphRenderSequence<float>>  (n, c),
                          RenderSequenceBuilder::build<GraphRenderSequence<double>> (n, c))
    {
    }

    void process (AudioBuffer<float>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        renderSequenceF.perform (audio, midi, playHead);
    }

    void process (AudioBuffer<double>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        renderSequenceD.perform (audio, midi, playHead);
    }

    void processIO (AudioGraphIOProcessor& io, AudioBuffer<float>& audio, MidiBuffer& midi)
    {
        processIOBlock (io, renderSequenceF, audio, midi);
    }

    void processIO (AudioGraphIOProcessor& io, AudioBuffer<double>& audio, MidiBuffer& midi)
    {
        processIOBlock (io, renderSequenceD, audio, midi);
    }

    int getLatencySamples() const { return latencySamples; }
    PrepareSettings getSettings() const { return settings; }

private:
    template <typename FloatType, typename SequenceType>
    static void processIOBlock (AudioGraphIOProcessor& io,
                                SequenceType& sequence,
                                AudioBuffer<FloatType>& buffer,
                                MidiBuffer& midiMessages)
    {
        switch (io.getType())
        {
            case AudioGraphIOProcessor::audioOutputNode:
            {
                auto&& currentAudioOutputBuffer = sequence.currentAudioOutputBuffer;

                for (int i = jmin (currentAudioOutputBuffer.getNumChannels(), buffer.getNumChannels()); --i >= 0;)
                    currentAudioOutputBuffer.addFrom (i, 0, buffer, i, 0, buffer.getNumSamples());

                break;
            }

            case AudioGraphIOProcessor::audioInputNode:
            {
                auto* currentInputBuffer = sequence.currentAudioInputBuffer;

                for (int i = jmin (currentInputBuffer->getNumChannels(), buffer.getNumChannels()); --i >= 0;)
                    buffer.copyFrom (i, 0, *currentInputBuffer, i, 0, buffer.getNumSamples());

                break;
            }

            case AudioGraphIOProcessor::midiOutputNode:
                sequence.currentMidiOutputBuffer.addEvents (midiMessages, 0, buffer.getNumSamples(), 0);
                break;

            case AudioGraphIOProcessor::midiInputNode:
                midiMessages.addEvents (*sequence.currentMidiInputBuffer, 0, buffer.getNumSamples(), 0);
                break;

            default:
                break;
        }
    }

    template <typename Float, typename Double>
    RenderSequence (PrepareSettings s, Float f, Double d)
        : settings (s),
          renderSequenceF (std::move (f.sequence)),
          renderSequenceD (std::move (d.sequence)),
          latencySamples (f.latencySamples)
    {
        jassert (f.latencySamples == d.latencySamples);

        renderSequenceF.prepareBuffers (settings.blockSize);
        renderSequenceD.prepareBuffers (settings.blockSize);
    }

    PrepareSettings settings;
    GraphRenderSequence<float>  renderSequenceF;
    GraphRenderSequence<double> renderSequenceD;
    int latencySamples = 0;
};

//==============================================================================
/*  Facilitates wait-free render-sequence updates.

    Topology updates always happen on the main thread (or synchronised with the main thread).
    After updating the graph, the 'baked' graph is passed to RenderSequenceExchange::set.
    At the top of the audio callback, RenderSequenceExchange::updateAudioThreadState will
    attempt to install the most-recently-baked graph, if there's one waiting.
*/
class RenderSequenceExchange : private Timer
{
public:
    RenderSequenceExchange()
    {
        startTimer (500);
    }

    ~RenderSequenceExchange() override
    {
        stopTimer();
    }

    void set (std::unique_ptr<RenderSequence>&& next)
    {
        const SpinLock::ScopedLockType lock (mutex);
        mainThreadState = std::move (next);
        isNew = true;
    }

    /** Call from the audio thread only. */
    void updateAudioThreadState()
    {
        const SpinLock::ScopedTryLockType lock (mutex);

        if (lock.isLocked() && isNew)
        {
            // Swap pointers rather than assigning to avoid calling delete here
            std::swap (mainThreadState, audioThreadState);
            isNew = false;
        }
    }

    /** Call from the audio thread only. */
    RenderSequence* getAudioThreadState() const { return audioThreadState.get(); }

private:
    void timerCallback() override
    {
        const SpinLock::ScopedLockType lock (mutex);

        if (! isNew)
            mainThreadState.reset();
    }

    SpinLock mutex;
    std::unique_ptr<RenderSequence> mainThreadState, audioThreadState;
    bool isNew = false;
};

//==============================================================================
AudioProcessorGraph::Connection::Connection (NodeAndChannel src, NodeAndChannel dst) noexcept
    : source (src), destination (dst)
{
}

bool AudioProcessorGraph::Connection::operator== (const Connection& other) const noexcept
{
    return source == other.source && destination == other.destination;
}

bool AudioProcessorGraph::Connection::operator!= (const Connection& c) const noexcept
{
    return ! operator== (c);
}

bool AudioProcessorGraph::Connection::operator< (const Connection& other) const noexcept
{
    const auto tie = [] (auto& x)
    {
        return std::tie (x.source.nodeID,
                         x.destination.nodeID,
                         x.source.channelIndex,
                         x.destination.channelIndex);
    };
    return tie (*this) < tie (other);
}

//==============================================================================
class AudioProcessorGraph::Pimpl : public AsyncUpdater
{
public:
    explicit Pimpl (AudioProcessorGraph& o) : owner (&o) {}

    ~Pimpl() override
    {
        cancelPendingUpdate();
        clear (UpdateKind::sync);
    }

    const auto& getNodes() const { return nodes.getNodes(); }

    void clear (UpdateKind updateKind)
    {
        if (getNodes().isEmpty())
            return;

        nodes = Nodes{};
        connections = Connections{};
        topologyChanged (updateKind);
    }

    auto getNodeForId (NodeID nodeID) const
    {
        return nodes.getNodeForId (nodeID);
    }

    Node::Ptr addNode (std::unique_ptr<AudioProcessor> newProcessor,
                       const NodeID nodeID,
                       UpdateKind updateKind)
    {
        if (newProcessor.get() == owner)
        {
            jassertfalse;
            return nullptr;
        }

        const auto idToUse = nodeID == NodeID() ? NodeID { ++(lastNodeID.uid) } : nodeID;

        auto added = nodes.addNode (std::move (newProcessor), idToUse);

        if (added == nullptr)
            return nullptr;

        if (lastNodeID < idToUse)
            lastNodeID = idToUse;

        setParentGraph (added->getProcessor());

        topologyChanged (updateKind);
        return added;
    }

    Node::Ptr removeNode (NodeID nodeID, UpdateKind updateKind)
    {
        connections.disconnectNode (nodeID);
        auto result = nodes.removeNode (nodeID);
        topologyChanged (updateKind);
        return result;
    }

    std::vector<Connection> getConnections() const
    {
        return connections.getConnections();
    }

    bool isConnected (const Connection& c) const
    {
        return connections.isConnected (c);
    }

    bool isConnected (NodeID srcID, NodeID destID) const
    {
        return connections.isConnected (srcID, destID);
    }

    bool isAnInputTo (const Node& src, const Node& dst) const
    {
        return isAnInputTo (src.nodeID, dst.nodeID);
    }

    bool isAnInputTo (NodeID src, NodeID dst) const
    {
        return connections.isAnInputTo (src, dst);
    }

    bool canConnect (const Connection& c) const
    {
        return connections.canConnect (nodes, c);
    }

    bool addConnection (const Connection& c, UpdateKind updateKind)
    {
        if (! connections.addConnection (nodes, c))
            return false;

        jassert (isConnected (c));
        topologyChanged (updateKind);
        return true;
    }

    bool removeConnection (const Connection& c, UpdateKind updateKind)
    {
        if (! connections.removeConnection (c))
            return false;

        topologyChanged (updateKind);
        return true;
    }

    bool disconnectNode (NodeID nodeID, UpdateKind updateKind)
    {
        if (! connections.disconnectNode (nodeID))
            return false;

        topologyChanged (updateKind);
        return true;
    }

    bool isConnectionLegal (const Connection& c) const
    {
        return connections.isConnectionLegal (nodes, c);
    }

    bool removeIllegalConnections (UpdateKind updateKind)
    {
        const auto result = connections.removeIllegalConnections (nodes);
        topologyChanged (updateKind);
        return result;
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int estimatedSamplesPerBlock)
    {
        owner->setRateAndBufferSizeDetails (sampleRate, estimatedSamplesPerBlock);

        PrepareSettings settings;
        settings.precision  = owner->getProcessingPrecision();
        settings.sampleRate = sampleRate;
        settings.blockSize  = estimatedSamplesPerBlock;

        nodeStates.setState (settings);

        topologyChanged (UpdateKind::sync);
    }

    void releaseResources()
    {
        nodeStates.setState (nullopt);
        topologyChanged (UpdateKind::sync);
    }

    void reset()
    {
        for (auto* n : getNodes())
            n->getProcessor()->reset();
    }

    void setNonRealtime (bool isProcessingNonRealtime)
    {
        for (auto* n : getNodes())
            n->getProcessor()->setNonRealtime (isProcessingNonRealtime);
    }

    template <typename Value>
    void processBlock (AudioBuffer<Value>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        renderSequenceExchange.updateAudioThreadState();

        if (renderSequenceExchange.getAudioThreadState() == nullptr && MessageManager::getInstance()->isThisTheMessageThread())
            handleAsyncUpdate();

        if (owner->isNonRealtime())
        {
            while (renderSequenceExchange.getAudioThreadState() == nullptr)
            {
                Thread::sleep (1);
                renderSequenceExchange.updateAudioThreadState();
            }
        }

        auto* state = renderSequenceExchange.getAudioThreadState();

        // Only process if the graph has the correct blockSize, sampleRate etc.
        if (state != nullptr && state->getSettings() == nodeStates.getLastRequestedSettings())
        {
            state->process (audio, midi, playHead);
        }
        else
        {
            audio.clear();
            midi.clear();
        }
    }

    /*  Call from the audio thread only. */
    auto* getAudioThreadState() const { return renderSequenceExchange.getAudioThreadState(); }

private:
    void setParentGraph (AudioProcessor* p) const
    {
        if (auto* ioProc = dynamic_cast<AudioGraphIOProcessor*> (p))
            ioProc->setParentGraph (owner);
    }

    void topologyChanged (UpdateKind updateKind)
    {
        owner->sendChangeMessage();

        if (updateKind == UpdateKind::sync && MessageManager::getInstance()->isThisTheMessageThread())
            handleAsyncUpdate();
        else
            triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        if (const auto newSettings = nodeStates.applySettings (nodes))
        {
            for (const auto node : nodes.getNodes())
                setParentGraph (node->getProcessor());

            auto sequence = std::make_unique<RenderSequence> (*newSettings, nodes, connections);
            owner->setLatencySamples (sequence->getLatencySamples());
            renderSequenceExchange.set (std::move (sequence));
        }
        else
        {
            renderSequenceExchange.set (nullptr);
        }
    }

    AudioProcessorGraph* owner = nullptr;
    Nodes nodes;
    Connections connections;
    NodeStates nodeStates;
    RenderSequenceExchange renderSequenceExchange;
    NodeID lastNodeID;
};

//==============================================================================
AudioProcessorGraph::AudioProcessorGraph() : pimpl (std::make_unique<Pimpl> (*this)) {}
AudioProcessorGraph::~AudioProcessorGraph() = default;

const String AudioProcessorGraph::getName() const                   { return "Audio Graph"; }
bool AudioProcessorGraph::supportsDoublePrecisionProcessing() const { return true; }
double AudioProcessorGraph::getTailLengthSeconds() const            { return 0; }
bool AudioProcessorGraph::acceptsMidi() const                       { return true; }
bool AudioProcessorGraph::producesMidi() const                      { return true; }
void AudioProcessorGraph::getStateInformation (MemoryBlock&)        {}
void AudioProcessorGraph::setStateInformation (const void*, int)    {}

void AudioProcessorGraph::processBlock (AudioBuffer<float>&  audio, MidiBuffer& midi)                       { return pimpl->processBlock (audio, midi, getPlayHead()); }
void AudioProcessorGraph::processBlock (AudioBuffer<double>& audio, MidiBuffer& midi)                       { return pimpl->processBlock (audio, midi, getPlayHead()); }
std::vector<AudioProcessorGraph::Connection> AudioProcessorGraph::getConnections() const                    { return pimpl->getConnections(); }
bool AudioProcessorGraph::addConnection (const Connection& c, UpdateKind updateKind)                        { return pimpl->addConnection (c, updateKind); }
bool AudioProcessorGraph::removeConnection (const Connection& c, UpdateKind updateKind)                     { return pimpl->removeConnection (c, updateKind); }
void AudioProcessorGraph::prepareToPlay (double sampleRate, int estimatedSamplesPerBlock)                   { return pimpl->prepareToPlay (sampleRate, estimatedSamplesPerBlock); }
void AudioProcessorGraph::clear (UpdateKind updateKind)                                                     { return pimpl->clear (updateKind); }
const ReferenceCountedArray<AudioProcessorGraph::Node>& AudioProcessorGraph::getNodes() const noexcept      { return pimpl->getNodes(); }
AudioProcessorGraph::Node* AudioProcessorGraph::getNodeForId (NodeID x) const                               { return pimpl->getNodeForId (x).get(); }
bool AudioProcessorGraph::disconnectNode (NodeID nodeID, UpdateKind updateKind)                             { return pimpl->disconnectNode (nodeID, updateKind); }
void AudioProcessorGraph::releaseResources()                                                                { return pimpl->releaseResources(); }
bool AudioProcessorGraph::removeIllegalConnections (UpdateKind updateKind)                                  { return pimpl->removeIllegalConnections (updateKind); }
void AudioProcessorGraph::reset()                                                                           { return pimpl->reset(); }
bool AudioProcessorGraph::canConnect (const Connection& c) const                                            { return pimpl->canConnect (c); }
bool AudioProcessorGraph::isConnected (const Connection& c) const noexcept                                  { return pimpl->isConnected (c); }
bool AudioProcessorGraph::isConnected (NodeID a, NodeID b) const noexcept                                   { return pimpl->isConnected (a, b); }
bool AudioProcessorGraph::isConnectionLegal (const Connection& c) const                                     { return pimpl->isConnectionLegal (c); }
bool AudioProcessorGraph::isAnInputTo (const Node& source, const Node& destination) const noexcept          { return pimpl->isAnInputTo (source, destination); }
bool AudioProcessorGraph::isAnInputTo (NodeID source, NodeID destination) const noexcept                    { return pimpl->isAnInputTo (source, destination); }

AudioProcessorGraph::Node::Ptr AudioProcessorGraph::addNode (std::unique_ptr<AudioProcessor> newProcessor,
                                                             NodeID nodeId,
                                                             UpdateKind updateKind)
{
    return pimpl->addNode (std::move (newProcessor), nodeId, updateKind);
}

void AudioProcessorGraph::setNonRealtime (bool isProcessingNonRealtime) noexcept
{
    AudioProcessor::setNonRealtime (isProcessingNonRealtime);
    pimpl->setNonRealtime (isProcessingNonRealtime);
}

AudioProcessorGraph::Node::Ptr AudioProcessorGraph::removeNode (NodeID nodeID, UpdateKind updateKind)
{
    return pimpl->removeNode (nodeID, updateKind);
}

AudioProcessorGraph::Node::Ptr AudioProcessorGraph::removeNode (Node* node, UpdateKind updateKind)
{
    if (node != nullptr)
        return removeNode (node->nodeID, updateKind);

    jassertfalse;
    return {};
}

//==============================================================================
AudioProcessorGraph::AudioGraphIOProcessor::AudioGraphIOProcessor (const IODeviceType deviceType)
    : type (deviceType)
{
}

AudioProcessorGraph::AudioGraphIOProcessor::~AudioGraphIOProcessor() = default;

const String AudioProcessorGraph::AudioGraphIOProcessor::getName() const
{
    switch (type)
    {
        case audioOutputNode:   return "Audio Output";
        case audioInputNode:    return "Audio Input";
        case midiOutputNode:    return "MIDI Output";
        case midiInputNode:     return "MIDI Input";
        default:                break;
    }

    return {};
}

void AudioProcessorGraph::AudioGraphIOProcessor::fillInPluginDescription (PluginDescription& d) const
{
    d.name = getName();
    d.category = "I/O devices";
    d.pluginFormatName = "Internal";
    d.manufacturerName = "JUCE";
    d.version = "1.0";
    d.isInstrument = false;

    d.deprecatedUid = d.uniqueId = d.name.hashCode();

    d.numInputChannels = getTotalNumInputChannels();

    if (type == audioOutputNode && graph != nullptr)
        d.numInputChannels = graph->getTotalNumInputChannels();

    d.numOutputChannels = getTotalNumOutputChannels();

    if (type == audioInputNode && graph != nullptr)
        d.numOutputChannels = graph->getTotalNumOutputChannels();
}

void AudioProcessorGraph::AudioGraphIOProcessor::prepareToPlay (double, int)
{
    jassert (graph != nullptr);
}

void AudioProcessorGraph::AudioGraphIOProcessor::releaseResources()
{
}

bool AudioProcessorGraph::AudioGraphIOProcessor::supportsDoublePrecisionProcessing() const
{
    return true;
}

void AudioProcessorGraph::AudioGraphIOProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midiMessages)
{
    jassert (graph != nullptr);

    if (auto* state = graph->pimpl->getAudioThreadState())
        state->processIO (*this, buffer, midiMessages);
}

void AudioProcessorGraph::AudioGraphIOProcessor::processBlock (AudioBuffer<double>& buffer, MidiBuffer& midiMessages)
{
    jassert (graph != nullptr);

    if (auto* state = graph->pimpl->getAudioThreadState())
        state->processIO (*this, buffer, midiMessages);
}

double AudioProcessorGraph::AudioGraphIOProcessor::getTailLengthSeconds() const
{
    return 0;
}

bool AudioProcessorGraph::AudioGraphIOProcessor::acceptsMidi() const
{
    return type == midiOutputNode;
}

bool AudioProcessorGraph::AudioGraphIOProcessor::producesMidi() const
{
    return type == midiInputNode;
}

bool AudioProcessorGraph::AudioGraphIOProcessor::isInput() const noexcept           { return type == audioInputNode  || type == midiInputNode; }
bool AudioProcessorGraph::AudioGraphIOProcessor::isOutput() const noexcept          { return type == audioOutputNode || type == midiOutputNode; }

bool AudioProcessorGraph::AudioGraphIOProcessor::hasEditor() const                  { return false; }
AudioProcessorEditor* AudioProcessorGraph::AudioGraphIOProcessor::createEditor()    { return nullptr; }

int AudioProcessorGraph::AudioGraphIOProcessor::getNumPrograms()                    { return 0; }
int AudioProcessorGraph::AudioGraphIOProcessor::getCurrentProgram()                 { return 0; }
void AudioProcessorGraph::AudioGraphIOProcessor::setCurrentProgram (int)            { }

const String AudioProcessorGraph::AudioGraphIOProcessor::getProgramName (int)       { return {}; }
void AudioProcessorGraph::AudioGraphIOProcessor::changeProgramName (int, const String&) {}

void AudioProcessorGraph::AudioGraphIOProcessor::getStateInformation (MemoryBlock&)     {}
void AudioProcessorGraph::AudioGraphIOProcessor::setStateInformation (const void*, int) {}

void AudioProcessorGraph::AudioGraphIOProcessor::setParentGraph (AudioProcessorGraph* const newGraph)
{
    graph = newGraph;

    if (graph != nullptr)
    {
        setPlayConfigDetails (type == audioOutputNode ? graph->getTotalNumOutputChannels() : 0,
                              type == audioInputNode  ? graph->getTotalNumInputChannels()  : 0,
                              getSampleRate(),
                              getBlockSize());

        updateHostDisplay();
    }
}

//==============================================================================
//==============================================================================
#if JUCE_UNIT_TESTS

class AudioProcessorGraphTests : public UnitTest
{
public:
    AudioProcessorGraphTests()
        : UnitTest ("AudioProcessorGraph", UnitTestCategories::audioProcessors) {}

    void runTest() override
    {
        const auto midiChannel = AudioProcessorGraph::midiChannelIndex;

        beginTest ("isConnected returns true when two nodes are connected");
        {
            AudioProcessorGraph graph;
            const auto nodeA = graph.addNode (BasicProcessor::make ({}, MidiIn::no, MidiOut::yes))->nodeID;
            const auto nodeB = graph.addNode (BasicProcessor::make ({}, MidiIn::yes, MidiOut::no))->nodeID;

            expect (graph.canConnect ({ { nodeA, midiChannel }, { nodeB, midiChannel } }));
            expect (! graph.canConnect ({ { nodeB, midiChannel }, { nodeA, midiChannel } }));
            expect (! graph.canConnect ({ { nodeA, midiChannel }, { nodeA, midiChannel } }));
            expect (! graph.canConnect ({ { nodeB, midiChannel }, { nodeB, midiChannel } }));

            expect (graph.getConnections().empty());
            expect (! graph.isConnected ({ { nodeA, midiChannel }, { nodeB, midiChannel } }));
            expect (! graph.isConnected (nodeA, nodeB));

            expect (graph.addConnection ({ { nodeA, midiChannel }, { nodeB, midiChannel } }));

            expect (graph.getConnections().size() == 1);
            expect (graph.isConnected ({ { nodeA, midiChannel }, { nodeB, midiChannel } }));
            expect (graph.isConnected (nodeA, nodeB));

            expect (graph.disconnectNode (nodeA));

            expect (graph.getConnections().empty());
            expect (! graph.isConnected ({ { nodeA, midiChannel }, { nodeB, midiChannel } }));
            expect (! graph.isConnected (nodeA, nodeB));
        }

        beginTest ("graph lookups work with a large number of connections");
        {
            AudioProcessorGraph graph;

            std::vector<AudioProcessorGraph::NodeID> nodeIDs;

            constexpr auto numNodes = 100;

            for (auto i = 0; i < numNodes; ++i)
            {
                nodeIDs.push_back (graph.addNode (BasicProcessor::make (BasicProcessor::getStereoProperties(),
                                                                        MidiIn::yes,
                                                                        MidiOut::yes))->nodeID);
            }

            for (auto it = nodeIDs.begin(); it != std::prev (nodeIDs.end()); ++it)
            {
                expect (graph.addConnection ({ { it[0], 0 }, { it[1], 0 } }));
                expect (graph.addConnection ({ { it[0], 1 }, { it[1], 1 } }));
            }

            // Check whether isConnected reports correct results when called
            // with both connections and nodes
            for (auto it = nodeIDs.begin(); it != std::prev (nodeIDs.end()); ++it)
            {
                expect (graph.isConnected ({ { it[0], 0 }, { it[1], 0 } }));
                expect (graph.isConnected ({ { it[0], 1 }, { it[1], 1 } }));
                expect (graph.isConnected (it[0], it[1]));
            }

            const auto& nodes = graph.getNodes();

            expect (! graph.isAnInputTo (*nodes[0], *nodes[0]));

            // Check whether isAnInputTo behaves correctly for a non-cyclic graph
            for (auto it = std::next (nodes.begin()); it != std::prev (nodes.end()); ++it)
            {
                expect (! graph.isAnInputTo (**it, **it));

                expect (graph.isAnInputTo (*nodes[0], **it));
                expect (! graph.isAnInputTo (**it, *nodes[0]));

                expect (graph.isAnInputTo (**it, *nodes[nodes.size() - 1]));
                expect (! graph.isAnInputTo (*nodes[nodes.size() - 1], **it));
            }

            // Make the graph cyclic
            graph.addConnection ({ { nodeIDs.back(), 0 }, { nodeIDs.front(), 0 } });
            graph.addConnection ({ { nodeIDs.back(), 1 }, { nodeIDs.front(), 1 } });

            // Check whether isAnInputTo behaves correctly for a cyclic graph
            for (const auto* node : graph.getNodes())
            {
                expect (graph.isAnInputTo (*node, *node));

                expect (graph.isAnInputTo (*nodes[0], *node));
                expect (graph.isAnInputTo (*node, *nodes[0]));

                expect (graph.isAnInputTo (*node, *nodes[nodes.size() - 1]));
                expect (graph.isAnInputTo (*nodes[nodes.size() - 1], *node));
            }
        }
    }

private:
    enum class MidiIn  { no, yes };
    enum class MidiOut { no, yes };

    class BasicProcessor  : public AudioProcessor
    {
    public:
        explicit BasicProcessor (const AudioProcessor::BusesProperties& layout, MidiIn mIn, MidiOut mOut)
            : AudioProcessor (layout), midiIn (mIn), midiOut (mOut) {}

        const String getName() const override                         { return "Basic Processor"; }
        double getTailLengthSeconds() const override                  { return {}; }
        bool acceptsMidi() const override                             { return midiIn  == MidiIn ::yes; }
        bool producesMidi() const override                            { return midiOut == MidiOut::yes; }
        AudioProcessorEditor* createEditor() override                 { return {}; }
        bool hasEditor() const override                               { return {}; }
        int getNumPrograms() override                                 { return 1; }
        int getCurrentProgram() override                              { return {}; }
        void setCurrentProgram (int) override                         {}
        const String getProgramName (int) override                    { return {}; }
        void changeProgramName (int, const String&) override          {}
        void getStateInformation (juce::MemoryBlock&) override        {}
        void setStateInformation (const void*, int) override          {}
        void prepareToPlay (double, int) override                     {}
        void releaseResources() override                              {}
        void processBlock (AudioBuffer<float>&, MidiBuffer&) override {}
        bool supportsDoublePrecisionProcessing() const override       { return true; }
        bool isMidiEffect() const override                            { return {}; }
        void reset() override                                         {}
        void setNonRealtime (bool) noexcept override                  {}

        using AudioProcessor::processBlock;

        static std::unique_ptr<AudioProcessor> make (const BusesProperties& layout,
                                                     MidiIn midiIn,
                                                     MidiOut midiOut)
        {
            return std::make_unique<BasicProcessor> (layout, midiIn, midiOut);
        }

        static BusesProperties getStereoProperties()
        {
            return BusesProperties().withInput ("in", AudioChannelSet::stereo())
                                    .withOutput ("out", AudioChannelSet::stereo());
        }

    private:
        MidiIn midiIn;
        MidiOut midiOut;
    };
};

static AudioProcessorGraphTests audioProcessorGraphTests;

#endif

} // namespace juce
