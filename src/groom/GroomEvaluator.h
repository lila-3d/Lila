// Adapted from GroomLab's groom_session.h/.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#pragma once

#include <groomlab/correctives.h>
#include <groomlab/flow.h>
#include <groomlab/follow_scalp.h>
#include <groomlab/graph.h>
#include <groomlab/graph_io.h>
#include <groomlab/groom.h>
#include <groomlab/hair_color.h>
#include <groomlab/parts.h>
#include <groomlab/paths.h>
#include <groomlab/scalp_maps.h>
#include <groomlab/sculpt.h>
#include <groomlab/simulation.h>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lila::groom
{
/** What an evaluation's errors say while the scalp has another topology than the groom was made on. */
inline constexpr const char* kScalpTopologyChanged =
    "the scalp's vertex order or triangles differ from the groom's: strands are rooted again by position";

/**
 * Keeps one groom evaluated: a GroomLab graph on a scalp with its guides, poses, correctives, parts, flow, painted maps,
 * colliders and the scalp's earlier frames. Every input is parsed or bound once per change, the graph's results are
 * cached between evaluations (an edit re-runs only the nodes it reaches), and the result is the same bit for bit
 * however it was reached: fresh, incrementally, on any thread count.
 *
 * Following the scalp: the graph evaluates on the scalp's rest pose and each strand is carried to the scalp now by its
 * root's frame. Scaling with the scalp: the groom is evaluated k times its stored size, k from the scalp's rest area (or
 * the host's transform scale) against a reference, so the stored graph and guides stay at 1x.
 *
 * Setters no-op on the same pointer, text or content, so a host may call them on every evaluation. One thread at a
 * time; the graph evaluates in parallel inside.
 */
class GroomEvaluator
{
public:
    struct Request
    {
        float density = 1.0f;
        /** Points the result may hold; 0: no limit. */
        double pointBudget = 0.0;
        /** A Strands node to evaluate instead of the output ("" or another kind of node: the output). */
        std::string viewNode;
        double time = 0.0;
        /** Interactive view: renderOnly nodes are bypassed and viewportOnly ones run; off, the other way round. */
        bool viewport = false;
        /** The Cards nodes build their cards on this evaluation. */
        bool buildCards = false;
        /** Result::bounds worked out (when the strands or guides changed); off, left as they were. */
        bool wantBounds = true;
    };

    struct Bounds
    {
        groomlab::Vec3f lo, hi;
        /** False: nothing to bound. */
        bool valid = false;
    };

    struct Message
    {
        std::string text;
        bool error = true;
    };

    /** Counters that move only when what they name changed (a cache hit leaves them). */
    struct Versions
    {
        std::uint64_t evaluation = 0;
        std::uint64_t strands = 0;
        std::uint64_t hierarchy = 0;
    };

    /** What the last evaluation did, by node name. */
    struct Stats
    {
        std::size_t strands = 0, points = 0, guides = 0;
        /** The HairColor and RenderData nodes the look comes from, and every color node making the output. */
        std::string hairColor, renderData;
        std::vector<std::string> colorNodes, colorBlends;
        /** Milliseconds per node in its last run, and how it ran ("cached" when it didn't). */
        std::map<std::string, double> times;
        std::map<std::string, std::string> runs;
        /** Each node's strand channels, from its cached result. */
        std::map<std::string, std::vector<std::string>> channels;
        std::map<std::string, std::string> notes;
        int scalpHistoryFrames = 0;
        std::size_t restEvaluations = 0;
        std::size_t cacheBytes = 0, cacheEvictions = 0;
    };

    struct Result
    {
        /** Null on failure. */
        groomlab::StrandsPtr strands;
        std::string viewedNode;
        /** The density the strands were evaluated at. */
        float density = 1.0f;
        /** The strands' and the guides' points. */
        Bounds bounds;
        /** The scalp as evaluated: vertex maps as color sets, refined by the Scalp nodes' subdivision. */
        groomlab::MeshPtr scalp;
        /** The same before it is refined. */
        groomlab::MeshPtr cageScalp;
        /** The guides on the scalp, posed and corrected (the preview while one is set). */
        groomlab::HierarchyPtr hierarchy;
        /** The guides as stored, rooted on the scalp. */
        groomlab::HierarchyPtr authoredHierarchy;
        groomlab::PartsPtr parts;
        groomlab::FlowPtr flow;
        Versions versions;
        std::vector<std::string> errors;
        /** To say once per change. */
        std::vector<Message> messages;
    };

    /** The default graph, no inputs. */
    GroomEvaluator();
    /** Throws GraphError for a bad graph, std::runtime_error for bad guide, part, flow, corrective or sculpt data. */
    explicit GroomEvaluator(groomlab::GroomData data);
    ~GroomEvaluator();
    GroomEvaluator(const GroomEvaluator&) = delete;
    GroomEvaluator& operator=(const GroomEvaluator&) = delete;

    // Stored inputs.

    /** Throws GraphError for a bad graph, leaving the evaluator as it was. The new graph takes the old one's cache. */
    void setGraphJson(const std::string& graphJson);
    /** No graph: evaluations make no strands and say `error`. Setting `graphJson` again does nothing. */
    void dropGraph(const std::string& graphJson, const std::string& error);
    bool hasGraph() const { return mDoc != nullptr; }
    const std::string& graphError() const { return mGraphError; }
    const std::string& hierarchyError() const { return mHierarchyError; }
    /** Guides as stored (GuideHierarchy::toJson; empty: none). A bad text keeps the error and no guides. */
    void setHierarchyJson(const std::string& json);
    void setHierarchy(std::shared_ptr<const groomlab::GuideHierarchy> hierarchy);
    /** The guides as stored at the groom's scale, not rooted. */
    std::shared_ptr<const groomlab::GuideHierarchy> storedHierarchy() const { return mParsedHierarchy; }
    void setPartsJson(const std::string& json);
    void setFlowJson(const std::string& json);
    void setCorrectivesJson(const std::string& json);
    /** Corrective layers with a slot take these weights on every frame (useLiveWeights). */
    void setCorrectiveWeights(std::map<int, float> weights);
    void clearCorrectiveWeights();
    /** The Sculpt nodes' edits (SculptSet JSON). */
    void setSculptsJson(const std::string& json);
    const groomlab::SculptSet& sculpts() const { return mSculpts; }
    /** The stored inputs that did not parse, "<label>: <why>". */
    std::vector<std::string> inputErrors() const;

    // Scalp and maps.

    /** World space at the evaluation time; may carry a rest pose and color sets. */
    void setScalp(groomlab::MeshPtr scalp);
    /** The scalp's points at another time on its topology; `restPoints` replace the rest pose. */
    void updateScalpPoints(std::vector<groomlab::Vec3f> points, const std::vector<groomlab::Vec3f>* restPoints = nullptr);
    const groomlab::MeshPtr& rawScalp() const { return mData.scalp; }
    /** The groom's other scalps by name, joined after its own (scalp_pieces.h). */
    void setScalps(groomlab::NamedScalps scalps);
    groomlab::MeshPtr joinedScalp();
    /** The topology the groom was made on; another one is said in every evaluation's errors. */
    void setScalpFingerprint(std::uint64_t fingerprint);
    /** The scalp as evaluated (maps laid on, refined). */
    groomlab::MeshPtr scalp();
    /** The scalp with its maps, not refined: what vertex maps are painted on. */
    groomlab::MeshPtr cageScalp();
    /** The Catmull-Clark levels the scalp is refined by (the most of scalpSubdivisions). */
    int scalpSubdivision() const;
    /** Each scalp's own levels by name ("scalp": the groom's own). */
    std::map<std::string, int> scalpSubdivisions() const;
    /** `joined` refined as the evaluator refines its scalp. */
    groomlab::MeshPtr refineAsScalp(const groomlab::MeshPtr& joined) const;
    void setScalpMapsJson(const std::string& json);
    void setScalpMaps(groomlab::ScalpMaps maps);
    const groomlab::ScalpMaps& scalpMaps() const { return mScalpMaps; }
    const std::string& scalpMapsError() const { return mScalpMapsError; }
    /** The map catalog when the scalp carries its vertex maps already; used while no scalp maps are set. */
    void setMapCatalog(groomlab::MapCatalog maps);

    // Moving inputs.

    /** Flat guides, world space, rooted. */
    void setGuides(groomlab::GuidesPtr guides);
    /** Hierarchy guides replaced by ID (simulated, cached). */
    void setGuidePoses(groomlab::GuidesPtr poses);
    groomlab::GuidesPtr guidePoses() const { return mData.guidePoses; }
    void setControls(std::map<std::string, float> controls);
    /** Meshes for ColliderMesh nodes by name; a null mesh removes the name. */
    void setMeshes(std::map<std::string, groomlab::MeshPtr> meshes);
    void setMesh(const std::string& name, groomlab::MeshPtr mesh);
    /** The scalp's earlier frames, for Jiggle nodes. */
    void setScalpHistory(groomlab::ScalpHistoryPtr history);
    void setPathResolver(groomlab::PathResolver resolver);
    void setPathResolver(std::shared_ptr<const groomlab::PathResolver> resolver);
    /** Off for one-shot evaluation: lower peak memory, nothing kept. */
    void setCacheIntermediates(bool enabled);
    /** Frames per second the time-dependent nodes count frames at (an NTSC rate made exact). */
    void setFramesPerSecond(double fps);
    double framesPerSecond() const { return mFps; }

    // Following the scalp.

    void setFollowScalp(bool on);
    bool followScalp() const { return mData.followScalp; }
    /** The last evaluation followed the scalp (on, and the scalp away from its rest pose). */
    bool following() const { return mFollowing; }

    // Scaling with the scalp.

    void setScaleSource(groomlab::ScaleSource source);
    groomlab::ScaleSource scaleSource() const { return mData.scaleSource; }
    void setScaleWithScalp(bool on);
    bool scaleWithScalp() const { return mData.scaleSource != groomlab::ScaleSource::Off; }
    /** The scalp's transform scale as the host measures it; 0 or less: none. Handed in before its scalp. */
    void setScaleTransform(double scale);
    double scaleTransform() const { return mScaleTransform; }
    void setScaleTransformReference(double scale);
    double scaleTransformReference() const
    {
        return mData.scaleTransformReference > 0.0 ? mData.scaleTransformReference : mFirstTransform;
    }
    /** A line for the user when the scalp's own area changed the groom's scale, and its serial (0: none yet). */
    const std::string& scaleNotice() const { return mScaleNotice; }
    std::uint64_t scaleNoticeSerial() const { return mScaleNoticeSerial; }
    /** The area the groom is 1x at; 0 or less: the first area measured. */
    void setScaleReference(double area);
    double scaleReference() const { return mData.scaleReference > 0.0 ? mData.scaleReference : mFirstArea; }
    double scaleArea() const { return mScaleArea; }
    /** k: what the groom is scaled by now. */
    double groomScale() const { return mGroomScale; }

    /** Whether the result depends on the time through the graph, the correctives or a scalp history. */
    bool readsTime() const;

    // Simulation (simulation.h).
    //
    // Each Simulate node keeps every frame from its start frame to the last one stepped. An evaluation at a time steps
    // a node whose cacheMode is "live" on to that frame from its last one when the frame is not cached; going back
    // reads the cache. A change of the node's settings, of the frame rate, or of what it is fed at a frame it shows
    // again drops its frames. Between two frames the two are blended; before its start frame, and while a preview is
    // set, a node passes its guides through.

    /** How the host's inputs at another time reach the evaluator (SimFeed); without one, frames on the way are stepped
     * in one go toward the inputs as they are. */
    void setSimulationFeed(groomlab::SimFeed feed) { mSimFeed = std::move(feed); }
    /** Off: an evaluation never steps a node; a frame neither cached nor baked shows the guides as groomed. */
    void setSimulationStepping(bool on) { mSimStepping = on; }
    /** The groom at a frame depends on earlier frames: a scalp history, or a Simulate node not covered by a bake that
     * matches its settings over max(firstFrame, its start) to lastFrame. */
    bool readsEarlierFrames(const std::string& viewNode, int firstFrame, int lastFrame) const;
    /** The Simulate nodes making the output (not bypassed), upstream first. */
    std::vector<std::string> simulateNodes() const;
    /** Every Simulate node making the output stepped on to `frame` from its last frame, in either cacheMode; the
     * inputs are put back as the last evaluation had them. */
    groomlab::SimRun simulateTo(int frame, const groomlab::SimProgress& progress = {});
    /** Steps the Simulate nodes an evaluation at `seconds` would step (live, stepping on, no bake covering it) on to the
     * last frame before `seconds` through the feed, so the frame at `seconds` is stepped from there as when playing.
     * The inputs are put back. */
    groomlab::SimRun simulateBefore(double seconds);
    /** Drops a node's frames ("": every node's); a bake stays. */
    void invalidateSimulation(const std::string& node = {});
    std::vector<groomlab::SimStatus> simulationStatus() const;
    /** A node's frame as simulated; null when not cached. */
    groomlab::GuidesPtr simulatedFrame(const std::string& node, int frame) const;
    /** A bake of a node; null removes it. While its stamp matches the node's settings its frames come from it. */
    void setBakedSimulation(const std::string& node, groomlab::BakedSimulationPtr baked);
    groomlab::BakedSimulationPtr bakedSimulation(const std::string& node) const;
    /** What a bake of the node made now records (SimSettings::hash of its params); 0 without the node. */
    std::uint64_t simulationStamp(const std::string& node) const;

    // Previews (a brush tool edits these between evaluations).

    /** Replaces the stored guides while set. `final`: shown as it is (poses and correctives already in it). */
    void setPreviewHierarchy(std::shared_ptr<groomlab::GuideHierarchy> preview, bool final = false);
    std::shared_ptr<groomlab::GuideHierarchy> previewHierarchy() const { return mPreviewHierarchy; }
    /** Replace the stored parts or flow while set (bound to the scalp as evaluated). */
    void setPreviewOverlays(std::shared_ptr<const groomlab::PartSet> parts, std::shared_ptr<const groomlab::FlowSet> flow);
    bool hasPreview() const { return mPreviewHierarchy != nullptr; }

    // Evaluation.

    /** Never throws: a failure leaves no strands and says why in errors. Valid until the next evaluate. */
    const Result& evaluate(const Request& request);
    const Result& last() const { return mResult; }
    /** The output at `density` and `seconds`. Throws what the evaluation threw. */
    groomlab::StrandsPtr evaluate(float density, double seconds, bool buildCards = false);
    /** One node by name ("" the output) with the last evaluation's inputs, in a scratch copy of the graph. */
    groomlab::Value evaluateNode(const std::string& name, float density, double seconds, bool buildCards = false);

    // What the last evaluation used and made.

    /** The guides on the scalp, posed then corrected; null without guides. */
    groomlab::HierarchyPtr hierarchy();
    /** Posed, no correctives: what shot sculpts are recorded against. */
    groomlab::HierarchyPtr posedHierarchy();
    /** The guides as stored, rooted on the scalp: no poses, correctives or preview. */
    groomlab::HierarchyPtr authoredHierarchy() { return rootedHierarchy(); }
    /** Earlier scalp frames the graph reads (the Jiggle nodes'). */
    int scalpHistoryFrames(const std::string& viewNode = {}) const;
    std::vector<double> scalpHistoryLoopFrames(double now, const std::string& viewNode = {}) const;
    groomlab::HairLook hairLook() const;
    /** The look as JSON for hosts: the HairLook's fields, its "lobes", and the params of the nodes it comes from. */
    std::string hairLookJson() const;
    Stats stats() const;
    std::vector<std::pair<std::string, double>> nodeTimes() const;
    std::vector<std::pair<std::string, std::string>> nodeNotes() const;
    /** Operator evaluations since the graph was set, and of those the incremental ones. */
    std::size_t evaluationCount() const;
    std::size_t incrementalCount() const;
    /** The rest pose's evaluations (pinned or fitted instances). */
    std::size_t restEvaluations() const;
    const groomlab::GroomData& data() const { return mData; }
    /** The document the last evaluation used, its cache as it is; valid until the next setGraphJson. */
    const groomlab::GraphDocument* viewDocument() const { return mDoc.get(); }

private:
    std::unique_ptr<groomlab::GraphDocument> parseScaled(const std::string& graphJson) const;
    void installGraph(std::unique_ptr<groomlab::GraphDocument> parsed);
    void refreshScale();
    void absorbScaleTransform();
    void scaleHierarchy();
    std::map<std::string, float> scaledControls() const;

    int scalpLevelsOn(const groomlab::MeshPtr& joined, groomlab::MeshPtr* levelled) const;
    groomlab::ScalpHistoryPtr historyJoined();
    void bindScalp(std::vector<std::string>& errors);
    void bindRefined();
    groomlab::HierarchyPtr rootedHierarchy();
    void updateActiveHierarchy();
    void applyCorrectives(groomlab::GuideHierarchy& guides, const groomlab::ScalpMesh* on) const;
    std::uint64_t correctivesKey(std::uint64_t key) const;
    void bindOverlays();
    void prepareFollow(std::vector<Message>& messages);
    void prepareTime(double seconds, std::vector<std::string>& errors, std::vector<Message>& messages);
    void fillExternals(groomlab::ExternalInputs& externals) const;
    groomlab::EvalContext context(groomlab::ExternalInputs& externals) const;
    groomlab::StrandsPtr pinToRest(const groomlab::StrandsPtr& strands, const std::string& viewedNode,
                                   const groomlab::EvalContext& ctx);
    groomlab::StrandsPtr carry(const groomlab::StrandsPtr& strands, int slot);
    const groomlab::PathResolver* paths() const;

    struct SimNode
    {
        groomlab::SimCache cache;
        groomlab::BakedSimulationPtr baked;
        /** Its frame for the last evaluation (the external "sim:<node>"); null: none. */
        groomlab::GuidesPtr shown;
        std::uint64_t shownHash = 0;
        /** Its inputs' hash when last checked at checkedSeconds: other inputs at the same time start it again. */
        bool checked = false;
        double checkedSeconds = 0.0;
        std::uint64_t checkedHash = 0;
        /** The inputs its last frame was stepped toward. */
        std::uint64_t towardHash = 0;
    };
    struct SimRef
    {
        std::string name;
        groomlab::NodeId id = 0;
        SimNode* node = nullptr;
    };
    std::vector<std::pair<std::string, groomlab::NodeId>> simulateNodeIds(const std::string& viewNode) const;
    std::vector<SimRef> simRefs(const std::string& viewNode);
    void checkSimInputs(SimRef& ref, double seconds, const groomlab::ExternalInputs& externals);
    std::pair<groomlab::GuidesPtr, std::uint64_t> simFrameAt(const SimNode& node, double seconds) const;
    groomlab::EvalContext simContext(double seconds, const groomlab::ExternalInputs& externals) const;
    groomlab::SimInputs simInputs(const SimRef& ref, const groomlab::EvalContext& ctx, std::uint64_t* hash);
    void stepSimulations(const Request& request, std::vector<std::string>& errors);
    void advanceSimulations(std::vector<SimRef>& nodes, int target, double nowSeconds, const groomlab::SimProgress& progress,
                            groomlab::SimRun& run);
    bool bakeUsable(const SimNode& node, double seconds) const;

    groomlab::GroomData mData;
    bool mCacheIntermediates = true;
    std::shared_ptr<const groomlab::PathResolver> mPaths;
    double mFps = 24.0;

    // The graph, parsed once per change at the groom's scale; mDoc is its clone that keeps the cache.
    std::unique_ptr<groomlab::GraphDocument> mParsed;
    std::unique_ptr<groomlab::GraphDocument> mDoc;
    std::unique_ptr<groomlab::RestPose> mRestPose;
    bool mGraphSet = false;
    std::string mGraphError;

    double mGroomScale = 1.0;
    double mScaleArea = 0.0;
    double mFirstArea = 0.0;
    double mScaleTransform = 0.0;
    double mFirstTransform = 0.0;
    double mMeasuredArea = 0.0, mMeasuredTransform = 0.0;
    bool mScaleMeasured = false;
    std::string mScaleNotice;
    std::uint64_t mScaleNoticeSerial = 0;
    std::map<std::string, double> mControlFactors;

    bool mHierarchySet = false;
    bool mHierarchyFromText = false;
    std::shared_ptr<const groomlab::GuideHierarchy> mRawHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mParsedHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mReboundHierarchy;
    std::shared_ptr<const groomlab::GuideHierarchy> mReboundFrom;
    std::uint64_t mReboundRest = 0;
    std::string mHierarchyError;
    std::uint64_t mParsedVersion = 0;
    std::uint64_t mReportedVersion = 0;
    std::uint64_t mRootedVersion = 0;
    std::uint64_t mRootedScalp = 0;
    bool mRooted = false;
    groomlab::HierarchyPtr mHierarchy;
    std::shared_ptr<groomlab::GuideHierarchy> mPreviewHierarchy;
    bool mPreviewFinal = false;
    groomlab::HierarchyPtr mActiveHierarchy;
    groomlab::HierarchyPtr mPosedHierarchy;
    std::uint64_t mActiveKey = 0;
    std::uint64_t mPosesHash = 0;
    bool mActiveValid = false;
    groomlab::Correctives mCorrectives;
    std::string mCorrectiveError;
    std::uint64_t mCorrectivesVersion = 0;
    bool mLiveWeights = false;
    std::map<int, float> mWeights;
    double mTime = 0.0;

    groomlab::SculptSet mSculpts;
    std::string mSculptError;
    std::map<std::string, std::pair<groomlab::SculptPtr, std::uint64_t>> mSculptExternals;

    groomlab::PartSet mPartSet;
    std::string mPartError;
    std::shared_ptr<const groomlab::PartSet> mPreviewParts;
    groomlab::PartsPtr mParts;
    std::uint64_t mPartsKey = 0;
    groomlab::FlowSet mFlowSet;
    std::string mFlowError;
    std::shared_ptr<const groomlab::FlowSet> mPreviewFlow;
    groomlab::FlowPtr mFlow;
    std::uint64_t mFlowKey = 0;
    const groomlab::PartSet* mShownParts = &mPartSet;
    const groomlab::FlowSet* mShownFlow = &mFlowSet;
    groomlab::PartSet mRefinedParts;
    std::uint64_t mRefinedPartsKey = 0;
    groomlab::FlowSet mRefinedFlow;
    std::uint64_t mRefinedFlowKey = 0;

    groomlab::MeshPtr mJoined, mJoinedMain;
    groomlab::NamedScalps mJoinedExtras;
    groomlab::ScalpHistoryPtr mJoinedHistory, mJoinedHistoryIn;
    groomlab::MeshPtr mJoinedHistoryOn;
    groomlab::MeshPtr mScalp;
    groomlab::MeshPtr mCageScalp;
    groomlab::MeshPtr mRefinedFrom;
    groomlab::GuidesPtr mGuides;
    groomlab::GuidesPtr mGuidesIn;
    std::uint64_t mGuidesOn = 0;
    groomlab::ScalpHistoryPtr mHistory;
    groomlab::ScalpHistoryPtr mHistoryIn;
    groomlab::MeshPtr mHistoryOn;
    std::uint64_t mMappedKey = 0;
    groomlab::MeshPtr mMappedFrom;
    std::uint64_t mMappedMapsKey = 0;
    std::vector<std::string> mMapErrors;
    bool mScalpMapsSet = false;
    std::string mScalpMapsText;
    bool mScalpMapsFromText = false;
    groomlab::ScalpMaps mScalpMaps;
    std::uint64_t mScalpMapsHash = 0;
    std::string mScalpMapsError;

    std::uint64_t mFingerprintKey = 0;
    bool mFingerprintDiffers = false;

    // Following the scalp: the inputs on the rest pose, each kept while what it comes from is.
    bool mFollowing = false;
    bool mFollowNoRestSaid = false;
    groomlab::MeshPtr mFollowRest;
    std::uint64_t mFollowRestKey = 0;
    groomlab::HierarchyPtr mFollowRooted;
    std::uint64_t mFollowRootedKey = 0;
    groomlab::HierarchyPtr mFollowActive;
    std::uint64_t mFollowActiveKey = 0;
    groomlab::PartsPtr mFollowParts;
    std::uint64_t mFollowPartsKey = 0;
    groomlab::FlowPtr mFollowFlow;
    std::uint64_t mFollowFlowKey = 0;
    groomlab::GuidesPtr mFollowGuidesIn, mFollowGuides;
    groomlab::MeshPtr mFollowGuidesOn;
    groomlab::ScalpHistoryPtr mFollowHistoryIn, mFollowHistory;
    groomlab::MeshPtr mFollowHistoryOn;
    struct Carried
    {
        groomlab::StrandsPtr in;
        groomlab::MeshPtr from, to;
        groomlab::StrandsPtr out;
    };
    /** The view's and evaluateNode's. */
    Carried mCarried[2];

    std::map<std::string, std::unique_ptr<SimNode>> mSims;
    groomlab::SimFeed mSimFeed;
    bool mSimStepping = true;
    /** The last evaluation's density and budget: what simulateTo evaluates inputs at. */
    float mSimDensity = 1.0f;
    double mSimPointBudget = 0.0;

    Result mResult;
    /** The strands or guides changed while no evaluation wanted bounds. */
    bool mBoundsStale = false;
    std::exception_ptr mFailure;
};
} // namespace lila::groom
