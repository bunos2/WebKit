/*
 * Copyright (C) 2014-2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(ASYNC_SCROLLING)
#include "AsyncScrollingCoordinator.h"

#include "DebugPageOverlays.h"
#include "DeprecatedGlobalSettings.h"
#include "Document.h"
#include "EditorClient.h"
#include "GraphicsLayer.h"
#include "LocalFrame.h"
#include "LocalFrameView.h"
#include "Logging.h"
#include "Page.h"
#include "PerformanceLoggingClient.h"
#include "RemoteFrame.h"
#include "RenderLayerCompositor.h"
#include "RenderView.h"
#include "ScrollAnimator.h"
#include "ScrollbarsController.h"
#include "ScrollingConstraints.h"
#include "ScrollingStateFixedNode.h"
#include "ScrollingStateFrameHostingNode.h"
#include "ScrollingStateFrameScrollingNode.h"
#include "ScrollingStateOverflowScrollProxyNode.h"
#include "ScrollingStateOverflowScrollingNode.h"
#include "ScrollingStatePositionedNode.h"
#include "ScrollingStateStickyNode.h"
#include "ScrollingStateTree.h"
#include "Settings.h"
#include "WheelEventTestMonitor.h"
#include "pal/HysteresisActivity.h"
#include <wtf/ProcessID.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

AsyncScrollingCoordinator::AsyncScrollingCoordinator(Page* page)
    : ScrollingCoordinator(page)
    , m_scrollingStateTree(makeUnique<ScrollingStateTree>(this))
    , m_hysterisisActivity([this](auto state) { hysterisisTimerFired(state); }, 200_ms)
{
}

void AsyncScrollingCoordinator::hysterisisTimerFired(PAL::HysteresisState state)
{
    if (RefPtr page = this->page(); page && state == PAL::HysteresisState::Stopped)
        page->didFinishScrolling();
}

AsyncScrollingCoordinator::~AsyncScrollingCoordinator() = default;

void AsyncScrollingCoordinator::scrollingStateTreePropertiesChanged()
{
    scheduleTreeStateCommit();
}

void AsyncScrollingCoordinator::scrollingThreadAddedPendingUpdate()
{
    scheduleRenderingUpdate();
}

#if PLATFORM(COCOA)
void AsyncScrollingCoordinator::handleWheelEventPhase(ScrollingNodeID nodeID, PlatformWheelEventPhase phase)
{
    ASSERT(isMainThread());

    if (!page())
        return;

    RefPtr frameView = frameViewForScrollingNode(nodeID);
    if (!frameView)
        return;

    if (nodeID == frameView->scrollingNodeID()) {
        frameView->scrollAnimator().handleWheelEventPhase(phase);
        return;
    }

    if (CheckedPtr scrollableArea = frameView->scrollableAreaForScrollingNodeID(nodeID))
        scrollableArea->scrollAnimator().handleWheelEventPhase(phase);
}
#endif

static inline void setStateScrollingNodeSnapOffsetsAsFloat(ScrollingStateScrollingNode& node, const LayoutScrollSnapOffsetsInfo* offsetInfo, float deviceScaleFactor)
{
    if (!offsetInfo) {
        node.setSnapOffsetsInfo(FloatScrollSnapOffsetsInfo());
        return;
    }

    // FIXME: Incorporate current page scale factor in snapping to device pixel. Perhaps we should just convert to float here and let UI process do the pixel snapping?
    node.setSnapOffsetsInfo(offsetInfo->convertUnits<FloatScrollSnapOffsetsInfo>(deviceScaleFactor));
}

void AsyncScrollingCoordinator::willCommitTree()
{
    updateEventTrackingRegions();
}

void AsyncScrollingCoordinator::updateEventTrackingRegions()
{
    if (!m_eventTrackingRegionsDirty)
        return;

    if (!m_scrollingStateTree->rootStateNode())
        return;

    m_scrollingStateTree->rootStateNode()->setEventTrackingRegions(absoluteEventTrackingRegions());
    m_eventTrackingRegionsDirty = false;
}

void AsyncScrollingCoordinator::frameViewLayoutUpdated(LocalFrameView& frameView)
{
    ASSERT(isMainThread());
    ASSERT(page());

    m_eventTrackingRegionsDirty = true;

    // If there isn't a root node yet, don't do anything. We'll be called again after creating one.
    if (!m_scrollingStateTree->rootStateNode())
        return;

    // We have to schedule a commit, but the computed non-fast region may not have actually changed.
    // FIXME: This needs to disambiguate between event regions in the scrolling tree, and those in GraphicsLayers.
    scheduleTreeStateCommit();

#if PLATFORM(COCOA)
    if (!coordinatesScrollingForFrameView(frameView))
        return;

    RefPtr page = frameView.frame().page();
    if (page && page->isMonitoringWheelEvents()) {
        RefPtr frameScrollingNode = dynamicDowncast<ScrollingStateFrameScrollingNode>(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()));
        if (!frameScrollingNode)
            return;

        frameScrollingNode->setIsMonitoringWheelEvents(page->isMonitoringWheelEvents());
    }
#else
    UNUSED_PARAM(frameView);
#endif
}

void AsyncScrollingCoordinator::frameViewVisualViewportChanged(LocalFrameView& frameView)
{
    ASSERT(isMainThread());
    ASSERT(page());

    if (!coordinatesScrollingForFrameView(frameView))
        return;
    
    // If the root layer does not have a ScrollingStateNode, then we should create one.
    RefPtr node = m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID());
    if (!node)
        return;

    auto& frameScrollingNode = downcast<ScrollingStateFrameScrollingNode>(*node);

    auto visualViewportIsSmallerThanLayoutViewport = [](const LocalFrameView& frameView) {
        auto layoutViewport = frameView.layoutViewportRect();
        auto visualViewport = frameView.visualViewportRect();
        return visualViewport.width() < layoutViewport.width() || visualViewport.height() < layoutViewport.height();
    };
    frameScrollingNode.setVisualViewportIsSmallerThanLayoutViewport(visualViewportIsSmallerThanLayoutViewport(frameView));
}

void AsyncScrollingCoordinator::frameViewWillBeDetached(LocalFrameView& frameView)
{
    ASSERT(isMainThread());
    ASSERT(page());

    if (!coordinatesScrollingForFrameView(frameView))
        return;

    RefPtr node = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()));
    if (!node)
        return;

    node->setScrollPosition(frameView.scrollPosition());
}

void AsyncScrollingCoordinator::updateIsMonitoringWheelEventsForFrameView(const LocalFrameView& frameView)
{
    RefPtr page = frameView.frame().page();
    if (!page)
        return;

    RefPtr node = dynamicDowncast<ScrollingStateFrameScrollingNode>(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()));
    if (!node)
        return;

    node->setIsMonitoringWheelEvents(page->isMonitoringWheelEvents());
}

void AsyncScrollingCoordinator::frameViewEventTrackingRegionsChanged(LocalFrameView& frameView)
{
    m_eventTrackingRegionsDirty = true;
    if (!m_scrollingStateTree->rootStateNode())
        return;

    // We have to schedule a commit, but the computed non-fast region may not have actually changed.
    // FIXME: This needs to disambiguate between event regions in the scrolling tree, and those in GraphicsLayers.
    scheduleTreeStateCommit();

    DebugPageOverlays::didChangeEventHandlers(frameView.protectedFrame());
}

void AsyncScrollingCoordinator::frameViewRootLayerDidChange(LocalFrameView& frameView)
{
    ASSERT(isMainThread());
    ASSERT(page());

    if (!coordinatesScrollingForFrameView(frameView))
        return;
    
    // FIXME: In some navigation scenarios, the FrameView has no RenderView or that RenderView has not been composited.
    // This needs cleaning up: https://bugs.webkit.org/show_bug.cgi?id=132724
    if (!frameView.scrollingNodeID())
        return;
    
    // If the root layer does not have a ScrollingStateNode, then we should create one.
    ensureRootStateNodeForFrameView(frameView);
    ASSERT(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()));

    ScrollingCoordinator::frameViewRootLayerDidChange(frameView);

    RefPtr node = dynamicDowncast<ScrollingStateFrameScrollingNode>(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()));
    if (!node)
        return;
    node->setScrollContainerLayer(scrollContainerLayerForFrameView(frameView));
    node->setScrolledContentsLayer(scrolledContentsLayerForFrameView(frameView));
    node->setRootContentsLayer(rootContentsLayerForFrameView(frameView));
    node->setCounterScrollingLayer(counterScrollingLayerForFrameView(frameView));
    node->setInsetClipLayer(insetClipLayerForFrameView(frameView));
    node->setContentShadowLayer(contentShadowLayerForFrameView(frameView));
    node->setHeaderLayer(headerLayerForFrameView(frameView));
    node->setFooterLayer(footerLayerForFrameView(frameView));
    node->setScrollBehaviorForFixedElements(frameView.scrollBehaviorForFixedElements());
    node->setVerticalScrollbarLayer(frameView.layerForVerticalScrollbar());
    node->setHorizontalScrollbarLayer(frameView.layerForHorizontalScrollbar());
}

bool AsyncScrollingCoordinator::requestStartKeyboardScrollAnimation(ScrollableArea& scrollableArea, const KeyboardScroll& scrollData)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return false;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return false;

    stateNode->setKeyboardScrollData({ KeyboardScrollAction::StartAnimation, scrollData });
    // FIXME: This should schedule a rendering update
    commitTreeStateIfNeeded();
    return true;
}

bool AsyncScrollingCoordinator::requestStopKeyboardScrollAnimation(ScrollableArea& scrollableArea, bool immediate)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return false;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return false;

    stateNode->setKeyboardScrollData({ immediate ? KeyboardScrollAction::StopImmediately : KeyboardScrollAction::StopWithAnimation, std::nullopt });
    // FIXME: This should schedule a rendering update
    commitTreeStateIfNeeded();
    return true;
}

bool AsyncScrollingCoordinator::requestScrollToPosition(ScrollableArea& scrollableArea, const ScrollPosition& scrollPosition, const ScrollPositionChangeOptions& options)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return false;

    RefPtr frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return false;

    if (!coordinatesScrollingForFrameView(*frameView))
        return false;

    setScrollingNodeScrollableAreaGeometry(scrollingNodeID, scrollableArea);

    bool inBackForwardCache = frameView->frame().document()->backForwardCacheState() != Document::NotInBackForwardCache;
    bool isSnapshotting = page()->isTakingSnapshotsForApplicationSuspension();
    bool inProgrammaticScroll = scrollableArea.currentScrollType() == ScrollType::Programmatic;

    if ((inProgrammaticScroll && options.animated == ScrollIsAnimated::No) || inBackForwardCache) {
        auto scrollUpdate = ScrollUpdate { scrollingNodeID, scrollPosition, { }, ScrollUpdateType::PositionUpdate, ScrollingLayerPositionAction::Set };
        applyScrollUpdate(WTFMove(scrollUpdate), ScrollType::Programmatic);
    }

    ASSERT(inProgrammaticScroll == (options.type == ScrollType::Programmatic));

    // If this frame view's document is being put into the back/forward cache, we don't want to update our
    // main frame scroll position. Just let the FrameView think that we did.
    if (isSnapshotting)
        return true;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return false;

    if (options.originalScrollDelta)
        stateNode->setRequestedScrollData({ ScrollRequestType::DeltaUpdate, *options.originalScrollDelta, options.type, options.clamping, options.animated });
    else
        stateNode->setRequestedScrollData({ ScrollRequestType::PositionUpdate, scrollPosition, options.type, options.clamping, options.animated });

    LOG_WITH_STREAM(Scrolling, stream << "AsyncScrollingCoordinator::requestScrollToPosition " << scrollPosition << " for nodeID " << scrollingNodeID << " requestedScrollData " << stateNode->requestedScrollData());

    // FIXME: This should schedule a rendering update
    commitTreeStateIfNeeded();
    return true;
}

void AsyncScrollingCoordinator::stopAnimatedScroll(ScrollableArea& scrollableArea)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return;

    RefPtr frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView || !coordinatesScrollingForFrameView(*frameView))
        return;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return;

    // Animated scrolls are always programmatic.
    stateNode->setRequestedScrollData({ ScrollRequestType::CancelAnimatedScroll, { } });
    // FIXME: This should schedule a rendering update
    commitTreeStateIfNeeded();
}

void AsyncScrollingCoordinator::setMouseIsOverScrollbar(Scrollbar* scrollbar, bool isOverScrollbar)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollbar->scrollableArea().scrollingNodeID();
    if (!scrollingNodeID)
        return;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return;
    stateNode->setScrollbarHoverState({ scrollbar->orientation() == ScrollbarOrientation::Vertical ? false : isOverScrollbar, scrollbar->orientation() == ScrollbarOrientation::Vertical ? isOverScrollbar : false });
}

void AsyncScrollingCoordinator::setMouseIsOverContentArea(ScrollableArea& scrollableArea, bool isOverContentArea)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return;
    stateNode->setMouseIsOverContentArea(isOverContentArea);
}

void AsyncScrollingCoordinator::setMouseMovedInContentArea(ScrollableArea& scrollableArea)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollableArea.scrollingNodeID();
    if (!scrollingNodeID)
        return;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return;

    auto mousePosition = scrollableArea.lastKnownMousePositionInView();
    auto horizontalScrollbar = scrollableArea.horizontalScrollbar();
    auto verticalScrollbar = scrollableArea.verticalScrollbar();

    MouseLocationState state = { horizontalScrollbar ? horizontalScrollbar->convertFromContainingView(mousePosition) : IntPoint(), verticalScrollbar ? verticalScrollbar->convertFromContainingView(mousePosition) : IntPoint() };
    stateNode->setMouseMovedInContentArea(state);
}

void AsyncScrollingCoordinator::setLayerHostingContextIdentifierForFrameHostingNode(ScrollingNodeID scrollingNodeID, std::optional<LayerHostingContextIdentifier> identifier)
{
    auto stateNode = dynamicDowncast<ScrollingStateFrameHostingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    ASSERT(stateNode);
    if (!stateNode)
        return;
    stateNode->setLayerHostingContextIdentifier(identifier);
}

void AsyncScrollingCoordinator::setScrollbarEnabled(Scrollbar& scrollbar)
{
    ASSERT(isMainThread());
    ASSERT(page());
    auto scrollingNodeID = scrollbar.scrollableArea().scrollingNodeID();
    if (!scrollingNodeID)
        return;

    auto stateNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(scrollingNodeID));
    if (!stateNode)
        return;
    stateNode->setScrollbarEnabledState(scrollbar.orientation(), scrollbar.enabled());
}

void AsyncScrollingCoordinator::applyScrollingTreeLayerPositions()
{
    m_scrollingTree->applyLayerPositions();
}

void AsyncScrollingCoordinator::synchronizeStateFromScrollingTree()
{
    ASSERT(isMainThread());
    applyPendingScrollUpdates();

    m_scrollingTree->traverseScrollingTree([&](ScrollingNodeID nodeID, ScrollingNodeType, std::optional<FloatPoint> scrollPosition, std::optional<FloatPoint> layoutViewportOrigin, bool scrolledSinceLastCommit) {
        if (scrollPosition && scrolledSinceLastCommit) {
            LOG_WITH_STREAM(Scrolling, stream << "AsyncScrollingCoordinator::synchronizeStateFromScrollingTree - node " << nodeID << " scroll position " << scrollPosition);
            updateScrollPositionAfterAsyncScroll(nodeID, scrollPosition.value(), layoutViewportOrigin, ScrollingLayerPositionAction::Set, ScrollType::User);
        }
    });
}

void AsyncScrollingCoordinator::applyPendingScrollUpdates()
{
    if (!m_scrollingTree)
        return;

    auto scrollUpdates = m_scrollingTree->takePendingScrollUpdates();
    for (auto& update : scrollUpdates) {
        LOG_WITH_STREAM(Scrolling, stream << "AsyncScrollingCoordinator::applyPendingScrollUpdates - node " << update.nodeID << " scroll position " << update.scrollPosition);
        applyScrollPositionUpdate(WTFMove(update), ScrollType::User);
    }
}

void AsyncScrollingCoordinator::scheduleRenderingUpdate()
{
    if (RefPtr page = this->page())
        page->scheduleRenderingUpdate(RenderingUpdateStep::ScrollingTreeUpdate);
}

LocalFrameView* AsyncScrollingCoordinator::frameViewForScrollingNode(LocalFrame& rootFrame, ScrollingNodeID scrollingNodeID) const
{
    ASSERT(rootFrame.isRootFrame());
    if (scrollingNodeID == m_scrollingStateTree->rootStateNode()->scrollingNodeID()) {
        if (rootFrame.view() && rootFrame.view()->scrollingNodeID() == scrollingNodeID)
            return rootFrame.view();
    }

    RefPtr stateNode = m_scrollingStateTree->stateNodeForID(scrollingNodeID);
    if (!stateNode)
        return nullptr;

    // Find the enclosing frame scrolling node.
    RefPtr parentNode = stateNode;
    while (parentNode && !parentNode->isFrameScrollingNode())
        parentNode = parentNode->parent();
    
    if (!parentNode)
        return nullptr;

    // Walk the frame tree to find the matching LocalFrameView. This is not ideal, but avoids back pointers to LocalFrameViews
    // from ScrollingTreeStateNodes.
    for (RefPtr<Frame> frame = &rootFrame; frame; frame = frame->tree().traverseNext()) {
        auto* localFrame = dynamicDowncast<LocalFrame>(frame.get());
        if (!localFrame)
            continue;
        if (auto* view = localFrame->view()) {
            if (view->scrollingNodeID() == parentNode->scrollingNodeID())
                return view;
        }
    }

    return nullptr;
}

LocalFrameView* AsyncScrollingCoordinator::frameViewForScrollingNode(ScrollingNodeID scrollingNodeID) const
{
    if (!m_scrollingStateTree->rootStateNode() || !page())
        return nullptr;
    for (const auto& rootFrame : page()->rootFrames()) {
        if (RefPtr frameView = frameViewForScrollingNode(rootFrame.get(), scrollingNodeID))
            return frameView.get();
    }
    return nullptr;
}

void AsyncScrollingCoordinator::applyScrollUpdate(ScrollUpdate&& update, ScrollType scrollType)
{
    applyPendingScrollUpdates();
    applyScrollPositionUpdate(WTFMove(update), scrollType);
}

void AsyncScrollingCoordinator::applyScrollPositionUpdate(ScrollUpdate&& update, ScrollType scrollType)
{
    switch (update.updateType) {
    case ScrollUpdateType::AnimatedScrollWillStart:
        animatedScrollWillStartForNode(update.nodeID);
        return;

    case ScrollUpdateType::AnimatedScrollDidEnd:
        animatedScrollDidEndForNode(update.nodeID);
        return;

    case ScrollUpdateType::WheelEventScrollWillStart:
        wheelEventScrollWillStartForNode(update.nodeID);
        return;

    case ScrollUpdateType::WheelEventScrollDidEnd:
        wheelEventScrollDidEndForNode(update.nodeID);
        return;

    case ScrollUpdateType::PositionUpdate:
        updateScrollPositionAfterAsyncScroll(update.nodeID, update.scrollPosition, update.layoutViewportOrigin, update.updateLayerPositionAction, scrollType);
        return;
    }
}

void AsyncScrollingCoordinator::animatedScrollWillStartForNode(ScrollingNodeID scrollingNodeID)
{
    ASSERT(isMainThread());

    RefPtr page = this->page();
    if (!page)
        return;

    auto* frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return;

    m_hysterisisActivity.start();
    page->willBeginScrolling();
}

void AsyncScrollingCoordinator::animatedScrollDidEndForNode(ScrollingNodeID scrollingNodeID)
{
    ASSERT(isMainThread());

    if (!page())
        return;

    RefPtr frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return;

    LOG_WITH_STREAM(Scrolling, stream << "AsyncScrollingCoordinator::animatedScrollDidEndForNode node " << scrollingNodeID);

    m_hysterisisActivity.stop();

    if (scrollingNodeID == frameView->scrollingNodeID()) {
        frameView->setScrollAnimationStatus(ScrollAnimationStatus::NotAnimating);
        return;
    }

    if (auto* scrollableArea = frameView->scrollableAreaForScrollingNodeID(scrollingNodeID)) {
        scrollableArea->setScrollAnimationStatus(ScrollAnimationStatus::NotAnimating);
        scrollableArea->animatedScrollDidEnd();
    }
}

void AsyncScrollingCoordinator::wheelEventScrollWillStartForNode(ScrollingNodeID scrollingNodeID)
{
    ASSERT(isMainThread());

    RefPtr page = this->page();
    if (!page)
        return;

    auto* frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return;

    m_hysterisisActivity.start();
    page->willBeginScrolling();
}

void AsyncScrollingCoordinator::wheelEventScrollDidEndForNode(ScrollingNodeID scrollingNodeID)
{
    ASSERT(isMainThread());

    if (!page())
        return;

    if (!frameViewForScrollingNode(scrollingNodeID))
        return;

    m_hysterisisActivity.stop();
}

void AsyncScrollingCoordinator::updateScrollPositionAfterAsyncScroll(ScrollingNodeID scrollingNodeID, const FloatPoint& scrollPosition, std::optional<FloatPoint> layoutViewportOrigin, ScrollingLayerPositionAction scrollingLayerPositionAction, ScrollType scrollType)
{
    ASSERT(isMainThread());

    RefPtr page = this->page();
    if (!page)
        return;

    RefPtr frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return;

    LOG_WITH_STREAM(Scrolling, stream << "AsyncScrollingCoordinator::updateScrollPositionAfterAsyncScroll node " << scrollingNodeID << " " << scrollType << " scrollPosition " << scrollPosition << " action " << scrollingLayerPositionAction);

    if (!frameView->frame().isMainFrame()) {
        if (scrollingLayerPositionAction == ScrollingLayerPositionAction::Set)
            page->editorClient().subFrameScrollPositionChanged();
    }

    if (scrollingNodeID == frameView->scrollingNodeID()) {
        reconcileScrollingState(*frameView, scrollPosition, layoutViewportOrigin, scrollType, ViewportRectStability::Stable, scrollingLayerPositionAction);
        return;
    }

    // Overflow-scroll area.
    if (CheckedPtr scrollableArea = frameView->scrollableAreaForScrollingNodeID(scrollingNodeID)) {
        auto previousScrollType = scrollableArea->currentScrollType();
        scrollableArea->setCurrentScrollType(scrollType);
        scrollableArea->notifyScrollPositionChanged(roundedIntPoint(scrollPosition));
        scrollableArea->setCurrentScrollType(previousScrollType);

        if (scrollingLayerPositionAction == ScrollingLayerPositionAction::Set)
            page->editorClient().overflowScrollPositionChanged();
    }
}

void AsyncScrollingCoordinator::reconcileScrollingState(LocalFrameView& frameView, const FloatPoint& scrollPosition, const LayoutViewportOriginOrOverrideRect& layoutViewportOriginOrOverrideRect, ScrollType scrollType, ViewportRectStability viewportRectStability, ScrollingLayerPositionAction scrollingLayerPositionAction)
{
    auto previousScrollType = frameView.currentScrollType();
    frameView.setCurrentScrollType(scrollType);

    LOG_WITH_STREAM(Scrolling, stream << getCurrentProcessID() << " AsyncScrollingCoordinator " << this << " reconcileScrollingState scrollPosition " << scrollPosition << " type " << scrollType << " stability " << viewportRectStability << " " << scrollingLayerPositionAction);

    std::optional<FloatRect> layoutViewportRect;

    WTF::switchOn(layoutViewportOriginOrOverrideRect,
        [&frameView](std::optional<FloatPoint> origin) {
            if (origin)
                frameView.setBaseLayoutViewportOrigin(LayoutPoint(origin.value()), LocalFrameView::TriggerLayoutOrNot::No);
        }, [&frameView, &layoutViewportRect, viewportRectStability](std::optional<FloatRect> overrideRect) {
            if (!overrideRect)
                return;

            layoutViewportRect = overrideRect;
            if (viewportRectStability != ViewportRectStability::ChangingObscuredInsetsInteractively)
                frameView.setLayoutViewportOverrideRect(LayoutRect(overrideRect.value()), viewportRectStability == ViewportRectStability::Stable ? LocalFrameView::TriggerLayoutOrNot::Yes : LocalFrameView::TriggerLayoutOrNot::No);
        }
    );

    frameView.setScrollClamping(ScrollClamping::Unclamped);
    frameView.notifyScrollPositionChanged(roundedIntPoint(scrollPosition));
    frameView.setScrollClamping(ScrollClamping::Clamped);

    frameView.setCurrentScrollType(previousScrollType);

    if (scrollType == ScrollType::User && scrollingLayerPositionAction != ScrollingLayerPositionAction::Set) {
        auto scrollingNodeID = frameView.scrollingNodeID();
        if (viewportRectStability == ViewportRectStability::Stable)
            reconcileViewportConstrainedLayerPositions(scrollingNodeID, frameView.rectForFixedPositionLayout(), scrollingLayerPositionAction);
        else if (layoutViewportRect)
            reconcileViewportConstrainedLayerPositions(scrollingNodeID, LayoutRect(layoutViewportRect.value()), scrollingLayerPositionAction);
    }

    if (!scrolledContentsLayerForFrameView(frameView))
        return;

    RefPtr counterScrollingLayer = counterScrollingLayerForFrameView(frameView);
    RefPtr insetClipLayer = insetClipLayerForFrameView(frameView);
    RefPtr contentShadowLayer = contentShadowLayerForFrameView(frameView);
    RefPtr rootContentsLayer = rootContentsLayerForFrameView(frameView);
    RefPtr headerLayer = headerLayerForFrameView(frameView);
    RefPtr footerLayer = footerLayerForFrameView(frameView);

    ASSERT(frameView.scrollPosition() == roundedIntPoint(scrollPosition));
    LayoutPoint scrollPositionForFixed = frameView.scrollPositionForFixedPosition();
    float topContentInset = frameView.topContentInset();

    FloatPoint positionForInsetClipLayer;
    if (insetClipLayer)
        positionForInsetClipLayer = FloatPoint(insetClipLayer->position().x(), LocalFrameView::yPositionForInsetClipLayer(scrollPosition, topContentInset));
    FloatPoint positionForContentsLayer = frameView.positionForRootContentLayer();
    
    FloatPoint positionForHeaderLayer = FloatPoint(scrollPositionForFixed.x(), LocalFrameView::yPositionForHeaderLayer(scrollPosition, topContentInset));
    FloatPoint positionForFooterLayer = FloatPoint(scrollPositionForFixed.x(),
        LocalFrameView::yPositionForFooterLayer(scrollPosition, topContentInset, frameView.totalContentsSize().height(), frameView.footerHeight()));

    if (scrollType == ScrollType::Programmatic || scrollingLayerPositionAction == ScrollingLayerPositionAction::Set) {
        reconcileScrollPosition(frameView, ScrollingLayerPositionAction::Set);

        if (counterScrollingLayer)
            counterScrollingLayer->setPosition(scrollPositionForFixed);
        if (insetClipLayer)
            insetClipLayer->setPosition(positionForInsetClipLayer);
        if (contentShadowLayer)
            contentShadowLayer->setPosition(positionForContentsLayer);
        if (rootContentsLayer)
            rootContentsLayer->setPosition(positionForContentsLayer);
        if (headerLayer)
            headerLayer->setPosition(positionForHeaderLayer);
        if (footerLayer)
            footerLayer->setPosition(positionForFooterLayer);
    } else {
        reconcileScrollPosition(frameView, ScrollingLayerPositionAction::Sync);

        if (counterScrollingLayer)
            counterScrollingLayer->syncPosition(scrollPositionForFixed);
        if (insetClipLayer)
            insetClipLayer->syncPosition(positionForInsetClipLayer);
        if (contentShadowLayer)
            contentShadowLayer->syncPosition(positionForContentsLayer);
        if (rootContentsLayer)
            rootContentsLayer->syncPosition(positionForContentsLayer);
        if (headerLayer)
            headerLayer->syncPosition(positionForHeaderLayer);
        if (footerLayer)
            footerLayer->syncPosition(positionForFooterLayer);
    }
}

void AsyncScrollingCoordinator::reconcileScrollPosition(LocalFrameView& frameView, ScrollingLayerPositionAction scrollingLayerPositionAction)
{
#if PLATFORM(IOS_FAMILY)
    // Doing all scrolling like this (UIScrollView style) would simplify code.
    auto* scrollContainerLayer = scrollContainerLayerForFrameView(frameView);
    if (!scrollContainerLayer)
        return;
    if (scrollingLayerPositionAction == ScrollingLayerPositionAction::Set)
        scrollContainerLayer->setBoundsOrigin(frameView.scrollPosition());
    else
        scrollContainerLayer->syncBoundsOrigin(frameView.scrollPosition());
#else
    // This uses scrollPosition because the root content layer accounts for scrollOrigin (see LocalFrameView::positionForRootContentLayer()).
    auto* scrolledContentsLayer = scrolledContentsLayerForFrameView(frameView);
    if (!scrolledContentsLayer)
        return;
    if (scrollingLayerPositionAction == ScrollingLayerPositionAction::Set)
        scrolledContentsLayer->setPosition(-frameView.scrollPosition());
    else
        scrolledContentsLayer->syncPosition(-frameView.scrollPosition());
#endif
}

void AsyncScrollingCoordinator::scrollBySimulatingWheelEventForTesting(ScrollingNodeID nodeID, FloatSize delta)
{
    if (m_scrollingTree)
        m_scrollingTree->scrollBySimulatingWheelEventForTesting(nodeID, delta);
}

void AsyncScrollingCoordinator::scrollableAreaScrollbarLayerDidChange(ScrollableArea& scrollableArea, ScrollbarOrientation orientation)
{
    ASSERT(isMainThread());
    ASSERT(page());

    RefPtr node = m_scrollingStateTree->stateNodeForID(scrollableArea.scrollingNodeID());
    if (is<ScrollingStateScrollingNode>(node)) {
        auto& scrollingNode = downcast<ScrollingStateScrollingNode>(*node);
        if (orientation == ScrollbarOrientation::Vertical)
            scrollingNode.setVerticalScrollbarLayer(scrollableArea.layerForVerticalScrollbar());
        else
            scrollingNode.setHorizontalScrollbarLayer(scrollableArea.layerForHorizontalScrollbar());
    }

    if (orientation == ScrollbarOrientation::Vertical)
        scrollableArea.verticalScrollbarLayerDidChange();
    else
        scrollableArea.horizontalScrollbarLayerDidChange();
}

ScrollingNodeID AsyncScrollingCoordinator::createNode(ScrollingNodeType nodeType, ScrollingNodeID newNodeID)
{
    LOG_WITH_STREAM(ScrollingTree, stream << "AsyncScrollingCoordinator::createNode " << nodeType << " node " << newNodeID);
    // TODO: rdar://123052250 Need a better way to fix scrolling tree in iframe process
    if ((!m_scrollingStateTree->rootStateNode() && nodeType == ScrollingNodeType::Subframe) || (m_scrollingStateTree->rootStateNode() && m_scrollingStateTree->rootStateNode()->scrollingNodeID() == newNodeID))
        return m_scrollingStateTree->insertNode(nodeType, newNodeID, { }, 0);
    return m_scrollingStateTree->createUnparentedNode(nodeType, newNodeID);
}

ScrollingNodeID AsyncScrollingCoordinator::insertNode(ScrollingNodeType nodeType, ScrollingNodeID newNodeID, ScrollingNodeID parentID, size_t childIndex)
{
    LOG_WITH_STREAM(ScrollingTree, stream << "AsyncScrollingCoordinator::insertNode " << nodeType << " node " << newNodeID << " parent " << parentID << " index " << childIndex);
    return m_scrollingStateTree->insertNode(nodeType, newNodeID, parentID, childIndex);
}

void AsyncScrollingCoordinator::unparentNode(ScrollingNodeID nodeID)
{
    m_scrollingStateTree->unparentNode(nodeID);
}

void AsyncScrollingCoordinator::unparentChildrenAndDestroyNode(ScrollingNodeID nodeID)
{
    m_scrollingStateTree->unparentChildrenAndDestroyNode(nodeID);
}

void AsyncScrollingCoordinator::detachAndDestroySubtree(ScrollingNodeID nodeID)
{
    m_scrollingStateTree->detachAndDestroySubtree(nodeID);
}

void AsyncScrollingCoordinator::clearAllNodes()
{
    m_scrollingStateTree->clear();
}

ScrollingNodeID AsyncScrollingCoordinator::parentOfNode(ScrollingNodeID nodeID) const
{
    auto scrollingNode = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!scrollingNode)
        return { };

    return scrollingNode->parentNodeID();
}

Vector<ScrollingNodeID> AsyncScrollingCoordinator::childrenOfNode(ScrollingNodeID nodeID) const
{
    auto scrollingNode = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!scrollingNode)
        return { };

    return scrollingNode->children().map([](auto& child) {
        return child->scrollingNodeID();
    });
}

void AsyncScrollingCoordinator::reconcileViewportConstrainedLayerPositions(ScrollingNodeID scrollingNodeID, const LayoutRect& viewportRect, ScrollingLayerPositionAction action)
{
    LOG_WITH_STREAM(Scrolling, stream << getCurrentProcessID() << " AsyncScrollingCoordinator::reconcileViewportConstrainedLayerPositions for viewport rect " << viewportRect << " and node " << scrollingNodeID);

    m_scrollingStateTree->reconcileViewportConstrainedLayerPositions(scrollingNodeID, viewportRect, action);
}

void AsyncScrollingCoordinator::ensureRootStateNodeForFrameView(LocalFrameView& frameView)
{
    ASSERT(frameView.scrollingNodeID());
    if (m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()))
        return;

    // For non-main frames, it is only possible to arrive in this function from
    // RenderLayerCompositor::updateBacking where the node has already been created.
    ASSERT(frameView.frame().isMainFrame());
    insertNode(ScrollingNodeType::MainFrame, frameView.scrollingNodeID(), { }, 0);
}

void AsyncScrollingCoordinator::setNodeLayers(ScrollingNodeID nodeID, const NodeLayers& nodeLayers)
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    ASSERT(node);
    if (!node)
        return;

    node->setLayer(nodeLayers.layer);

    if (is<ScrollingStateScrollingNode>(node)) {
        auto& scrollingNode = downcast<ScrollingStateScrollingNode>(*node);
        scrollingNode.setScrollContainerLayer(nodeLayers.scrollContainerLayer);
        scrollingNode.setScrolledContentsLayer(nodeLayers.scrolledContentsLayer);
        scrollingNode.setHorizontalScrollbarLayer(nodeLayers.horizontalScrollbarLayer);
        scrollingNode.setVerticalScrollbarLayer(nodeLayers.verticalScrollbarLayer);

        if (is<ScrollingStateFrameScrollingNode>(node)) {
            auto& frameScrollingNode = downcast<ScrollingStateFrameScrollingNode>(*node);
            frameScrollingNode.setInsetClipLayer(nodeLayers.insetClipLayer);
            frameScrollingNode.setCounterScrollingLayer(nodeLayers.counterScrollingLayer);
            frameScrollingNode.setRootContentsLayer(nodeLayers.rootContentsLayer);
        }
    }
}

void AsyncScrollingCoordinator::setFrameScrollingNodeState(ScrollingNodeID nodeID, const LocalFrameView& frameView)
{
    auto stateNode = m_scrollingStateTree->stateNodeForID(nodeID);
    ASSERT(stateNode);
    if (!is<ScrollingStateFrameScrollingNode>(stateNode))
        return;

    auto& settings = page()->mainFrame().settings();
    auto& frameScrollingNode = downcast<ScrollingStateFrameScrollingNode>(*stateNode);

    frameScrollingNode.setFrameScaleFactor(frameView.frame().frameScaleFactor());
    frameScrollingNode.setHeaderHeight(frameView.headerHeight());
    frameScrollingNode.setFooterHeight(frameView.footerHeight());
    frameScrollingNode.setTopContentInset(frameView.topContentInset());
    frameScrollingNode.setLayoutViewport(frameView.layoutViewportRect());
    frameScrollingNode.setAsyncFrameOrOverflowScrollingEnabled(settings.asyncFrameScrollingEnabled() || settings.asyncOverflowScrollingEnabled());
    frameScrollingNode.setScrollingPerformanceTestingEnabled(settings.scrollingPerformanceTestingEnabled());
    frameScrollingNode.setOverlayScrollbarsEnabled(WebCore::DeprecatedGlobalSettings::usesOverlayScrollbars());
    frameScrollingNode.setWheelEventGesturesBecomeNonBlocking(settings.wheelEventGesturesBecomeNonBlocking());

    frameScrollingNode.setMinLayoutViewportOrigin(frameView.minStableLayoutViewportOrigin());
    frameScrollingNode.setMaxLayoutViewportOrigin(frameView.maxStableLayoutViewportOrigin());

    if (auto visualOverrideRect = frameView.visualViewportOverrideRect())
        frameScrollingNode.setOverrideVisualViewportSize(FloatSize(visualOverrideRect.value().size()));
    else
        frameScrollingNode.setOverrideVisualViewportSize(std::nullopt);

    frameScrollingNode.setFixedElementsLayoutRelativeToFrame(frameView.fixedElementsLayoutRelativeToFrame());

    auto visualViewportIsSmallerThanLayoutViewport = [](const LocalFrameView& frameView) {
        auto layoutViewport = frameView.layoutViewportRect();
        auto visualViewport = frameView.visualViewportRect();
        return visualViewport.width() < layoutViewport.width() || visualViewport.height() < layoutViewport.height();
    };
    frameScrollingNode.setVisualViewportIsSmallerThanLayoutViewport(visualViewportIsSmallerThanLayoutViewport(frameView));
    
    frameScrollingNode.setScrollBehaviorForFixedElements(frameView.scrollBehaviorForFixedElements());
}

void AsyncScrollingCoordinator::setScrollingNodeScrollableAreaGeometry(ScrollingNodeID nodeID, ScrollableArea& scrollableArea)
{
    auto scrollingNode = dynamicDowncast<ScrollingStateScrollingNode>(m_scrollingStateTree->stateNodeForID(nodeID));
    if (!scrollingNode)
        return;

    auto* verticalScrollbar = scrollableArea.verticalScrollbar();
    auto* horizontalScrollbar = scrollableArea.horizontalScrollbar();
    scrollingNode->setScrollerImpsFromScrollbars(verticalScrollbar, horizontalScrollbar);
    if (horizontalScrollbar)
        scrollingNode->setScrollbarEnabledState(ScrollbarOrientation::Horizontal, horizontalScrollbar->enabled());
    if (verticalScrollbar)
        scrollingNode->setScrollbarEnabledState(ScrollbarOrientation::Vertical, verticalScrollbar->enabled());

    scrollingNode->setScrollOrigin(scrollableArea.scrollOrigin());
    scrollingNode->setScrollPosition(scrollableArea.scrollPosition());
    scrollingNode->setTotalContentsSize(scrollableArea.totalContentsSize());
    scrollingNode->setReachableContentsSize(scrollableArea.reachableTotalContentsSize());
    scrollingNode->setScrollableAreaSize(scrollableArea.visibleSize());

    ScrollableAreaParameters scrollParameters;
    scrollParameters.horizontalScrollElasticity = scrollableArea.horizontalOverscrollBehavior() == OverscrollBehavior::None ? ScrollElasticity::None : scrollableArea.horizontalScrollElasticity();
    scrollParameters.verticalScrollElasticity = scrollableArea.verticalOverscrollBehavior() == OverscrollBehavior::None ? ScrollElasticity::None : scrollableArea.verticalScrollElasticity();
    scrollParameters.allowsHorizontalScrolling = scrollableArea.allowsHorizontalScrolling();
    scrollParameters.allowsVerticalScrolling = scrollableArea.allowsVerticalScrolling();
    scrollParameters.horizontalOverscrollBehavior = scrollableArea.horizontalOverscrollBehavior();
    scrollParameters.verticalOverscrollBehavior = scrollableArea.verticalOverscrollBehavior();
    scrollParameters.horizontalScrollbarMode = scrollableArea.horizontalScrollbarMode();
    scrollParameters.verticalScrollbarMode = scrollableArea.verticalScrollbarMode();
    scrollParameters.horizontalNativeScrollbarVisibility = scrollableArea.horizontalNativeScrollbarVisibility();
    scrollParameters.verticalNativeScrollbarVisibility = scrollableArea.verticalNativeScrollbarVisibility();
    scrollParameters.useDarkAppearanceForScrollbars = scrollableArea.useDarkAppearanceForScrollbars();
    scrollParameters.scrollbarWidthStyle = scrollableArea.scrollbarWidthStyle();

    scrollingNode->setScrollableAreaParameters(scrollParameters);

    scrollableArea.updateSnapOffsets();
    setStateScrollingNodeSnapOffsetsAsFloat(*scrollingNode, scrollableArea.snapOffsetsInfo(), page()->deviceScaleFactor());
    scrollingNode->setCurrentHorizontalSnapPointIndex(scrollableArea.currentHorizontalSnapPointIndex());
    scrollingNode->setCurrentVerticalSnapPointIndex(scrollableArea.currentVerticalSnapPointIndex());
}

void AsyncScrollingCoordinator::setViewportConstraintedNodeConstraints(ScrollingNodeID nodeID, const ViewportConstraints& constraints)
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!node)
        return;

    switch (constraints.constraintType()) {
    case ViewportConstraints::FixedPositionConstraint: {
        auto& fixedNode = downcast<ScrollingStateFixedNode>(*node);
        fixedNode.updateConstraints((const FixedPositionViewportConstraints&)constraints);
        break;
    }
    case ViewportConstraints::StickyPositionConstraint: {
        auto& stickyNode = downcast<ScrollingStateStickyNode>(*node);
        stickyNode.updateConstraints((const StickyPositionViewportConstraints&)constraints);
        break;
    }
    }
}

void AsyncScrollingCoordinator::setPositionedNodeConstraints(ScrollingNodeID nodeID, const AbsolutePositionConstraints& constraints)
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!node)
        return;

    ASSERT(is<ScrollingStatePositionedNode>(*node));
    if (auto* positionedNode = dynamicDowncast<ScrollingStatePositionedNode>(*node))
        positionedNode->updateConstraints(constraints);
}

void AsyncScrollingCoordinator::setRelatedOverflowScrollingNodes(ScrollingNodeID nodeID, Vector<ScrollingNodeID>&& relatedNodes)
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!node)
        return;

    if (is<ScrollingStatePositionedNode>(node))
        downcast<ScrollingStatePositionedNode>(*node).setRelatedOverflowScrollingNodes(WTFMove(relatedNodes));
    else if (is<ScrollingStateOverflowScrollProxyNode>(node)) {
        auto& overflowScrollProxyNode = downcast<ScrollingStateOverflowScrollProxyNode>(*node);
        if (!relatedNodes.isEmpty())
            overflowScrollProxyNode.setOverflowScrollingNode(relatedNodes[0]);
        else
            overflowScrollProxyNode.setOverflowScrollingNode({ });
    } else
        ASSERT_NOT_REACHED();
}

void AsyncScrollingCoordinator::setSynchronousScrollingReasons(ScrollingNodeID nodeID, OptionSet<SynchronousScrollingReason> reasons)
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!is<ScrollingStateScrollingNode>(node))
        return;

    auto& scrollingStateNode = downcast<ScrollingStateScrollingNode>(*node);
    if (reasons && is<ScrollingStateFrameScrollingNode>(scrollingStateNode)) {
        // The LocalFrameView's GraphicsLayer is likely to be out-of-synch with the PlatformLayer
        // at this point. So we'll update it before we switch back to main thread scrolling
        // in order to avoid layer positioning bugs.
        if (RefPtr frameView = frameViewForScrollingNode(nodeID))
            reconcileScrollPosition(*frameView, ScrollingLayerPositionAction::Set);
    }

    // FIXME: Ideally all the "synchronousScrollingReasons" functions should be #ifdeffed.
#if ENABLE(SCROLLING_THREAD)
    scrollingStateNode.setSynchronousScrollingReasons(reasons);
#endif
}

OptionSet<SynchronousScrollingReason> AsyncScrollingCoordinator::synchronousScrollingReasons(ScrollingNodeID nodeID) const
{
    RefPtr node = m_scrollingStateTree->stateNodeForID(nodeID);
    if (!is<ScrollingStateScrollingNode>(node))
        return { };

#if ENABLE(SCROLLING_THREAD)
    return downcast<ScrollingStateScrollingNode>(*node).synchronousScrollingReasons();
#else
    return { };
#endif
}

void AsyncScrollingCoordinator::windowScreenDidChange(PlatformDisplayID displayID, std::optional<FramesPerSecond> nominalFramesPerSecond)
{
    if (m_scrollingTree)
        m_scrollingTree->windowScreenDidChange(displayID, nominalFramesPerSecond);
}

bool AsyncScrollingCoordinator::hasSubscrollers() const
{
    return m_scrollingStateTree && m_scrollingStateTree->scrollingNodeCount() > 1;
}

bool AsyncScrollingCoordinator::isUserScrollInProgress(ScrollingNodeID nodeID) const
{
    if (m_scrollingTree)
        return m_scrollingTree->isUserScrollInProgressForNode(nodeID);

    return false;
}

bool AsyncScrollingCoordinator::isRubberBandInProgress(ScrollingNodeID nodeID) const
{
    if (m_scrollingTree)
        return m_scrollingTree->isRubberBandInProgressForNode(nodeID);

    return false;
}

void AsyncScrollingCoordinator::setScrollPinningBehavior(ScrollPinningBehavior pinning)
{
    scrollingTree()->setScrollPinningBehavior(pinning);
}

ScrollingNodeID AsyncScrollingCoordinator::scrollableContainerNodeID(const RenderObject& renderer) const
{
    if (auto overflowScrollingNodeID = renderer.view().compositor().asyncScrollableContainerNodeID(renderer))
        return overflowScrollingNodeID;

    // If we're in a scrollable frame, return that.
    RefPtr frameView = renderer.frame().view();
    if (!frameView)
        return { };

    if (auto scrollingNodeID = frameView->scrollingNodeID())
        return scrollingNodeID;

    // Otherwise, look for a scrollable element in the containing frame.
    if (RefPtr ownerElement = renderer.document().ownerElement()) {
        if (CheckedPtr frameRenderer = ownerElement->renderer())
            return scrollableContainerNodeID(*frameRenderer);
    }

    return { };
}

String AsyncScrollingCoordinator::scrollingStateTreeAsText(OptionSet<ScrollingStateTreeAsTextBehavior> behavior) const
{
    if (m_scrollingStateTree->rootStateNode()) {
        if (m_eventTrackingRegionsDirty)
            m_scrollingStateTree->rootStateNode()->setEventTrackingRegions(absoluteEventTrackingRegions());
        return m_scrollingStateTree->scrollingStateTreeAsText(behavior);
    }

    return emptyString();
}

String AsyncScrollingCoordinator::scrollingTreeAsText(OptionSet<ScrollingStateTreeAsTextBehavior> behavior) const
{
    if (!m_scrollingTree)
        return emptyString();

    return m_scrollingTree->scrollingTreeAsText(behavior);
}

bool AsyncScrollingCoordinator::haveScrollingTree() const
{
    return !!m_scrollingTree;
}

void AsyncScrollingCoordinator::setActiveScrollSnapIndices(ScrollingNodeID scrollingNodeID, std::optional<unsigned> horizontalIndex, std::optional<unsigned> verticalIndex)
{
    ASSERT(isMainThread());
    
    if (!page())
        return;
    
    RefPtr frameView = frameViewForScrollingNode(scrollingNodeID);
    if (!frameView)
        return;
    
    if (CheckedPtr scrollableArea = frameView->scrollableAreaForScrollingNodeID(scrollingNodeID)) {
        scrollableArea->setCurrentHorizontalSnapPointIndex(horizontalIndex);
        scrollableArea->setCurrentVerticalSnapPointIndex(verticalIndex);
    }
}

bool AsyncScrollingCoordinator::isScrollSnapInProgress(ScrollingNodeID nodeID) const
{
    if (m_scrollingTree)
        return m_scrollingTree->isScrollSnapInProgressForNode(nodeID);

    return false;
}

void AsyncScrollingCoordinator::updateScrollSnapPropertiesWithFrameView(const LocalFrameView& frameView)
{
    if (RefPtr node = dynamicDowncast<ScrollingStateFrameScrollingNode>(m_scrollingStateTree->stateNodeForID(frameView.scrollingNodeID()))) {
        setStateScrollingNodeSnapOffsetsAsFloat(*node, frameView.snapOffsetsInfo(), page()->deviceScaleFactor());
        node->setCurrentHorizontalSnapPointIndex(frameView.currentHorizontalSnapPointIndex());
        node->setCurrentVerticalSnapPointIndex(frameView.currentVerticalSnapPointIndex());
    }
}

void AsyncScrollingCoordinator::reportExposedUnfilledArea(MonotonicTime timestamp, unsigned unfilledArea)
{
    if (RefPtr page = this->page(); page && page->performanceLoggingClient())
        page->performanceLoggingClient()->logScrollingEvent(PerformanceLoggingClient::ScrollingEvent::ExposedTilelessArea, timestamp, unfilledArea);
}

void AsyncScrollingCoordinator::reportSynchronousScrollingReasonsChanged(MonotonicTime timestamp, OptionSet<SynchronousScrollingReason> reasons)
{
    if (RefPtr page = this->page(); page && page->performanceLoggingClient())
        page->performanceLoggingClient()->logScrollingEvent(PerformanceLoggingClient::ScrollingEvent::SwitchedScrollingMode, timestamp, reasons.toRaw());
}

bool AsyncScrollingCoordinator::scrollAnimatorEnabled() const
{
    ASSERT(isMainThread());
    auto* localMainFrame = dynamicDowncast<LocalFrame>(page()->mainFrame());
    if (!localMainFrame)
        return false;
    auto& settings = localMainFrame->settings();
    return settings.scrollAnimatorEnabled();
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING)
