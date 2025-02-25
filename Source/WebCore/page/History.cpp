/*
 * Copyright (C) 2007-2018 Apple Inc.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "History.h"

#include "BackForwardController.h"
#include "Document.h"
#include "DocumentInlines.h"
#include "FrameLoader.h"
#include "HistoryController.h"
#include "HistoryItem.h"
#include "LocalFrame.h"
#include "LocalFrameLoaderClient.h"
#include "Logging.h"
#include "NavigationScheduler.h"
#include "OriginAccessPatterns.h"
#include "Page.h"
#include "Quirks.h"
#include "ScriptController.h"
#include "SecurityOrigin.h"
#include <wtf/CheckedArithmetic.h>
#include <wtf/IsoMallocInlines.h>
#include <wtf/MainThread.h>
#include <wtf/text/StringConcatenateNumbers.h>

#if PLATFORM(COCOA)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(History);

History::History(LocalDOMWindow& window)
    : LocalDOMWindowProperty(&window)
{
}

static bool isDocumentFullyActive(LocalFrame* frame)
{
    return frame && frame->document()->isFullyActive();
}

static Exception documentNotFullyActive()
{
    return Exception { ExceptionCode::SecurityError, "Attempt to use History API from a document that isn't fully active"_s };
}

ExceptionOr<unsigned> History::length() const
{
    auto* frame = this->frame();
    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();
    auto* page = frame->page();
    if (!page)
        return 0;
    return page->backForward().count();
}

ExceptionOr<History::ScrollRestoration> History::scrollRestoration() const
{
    auto* frame = this->frame();
    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();

    auto* historyItem = frame->loader().history().currentItem();
    if (!historyItem)
        return ScrollRestoration::Auto;
    
    return historyItem->shouldRestoreScrollPosition() ? ScrollRestoration::Auto : ScrollRestoration::Manual;
}

ExceptionOr<void> History::setScrollRestoration(ScrollRestoration scrollRestoration)
{
    auto* frame = this->frame();
    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();

    auto* historyItem = frame->loader().history().currentItem();
    if (historyItem)
        historyItem->setShouldRestoreScrollPosition(scrollRestoration == ScrollRestoration::Auto);

    return { };
}

ExceptionOr<SerializedScriptValue*> History::state()
{
    auto* frame = this->frame();
    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();
    m_lastStateObjectRequested = stateInternal();
    return m_lastStateObjectRequested.get();
}

SerializedScriptValue* History::stateInternal() const
{
    auto* frame = this->frame();
    if (!frame)
        return nullptr;
    auto* historyItem = frame->loader().history().currentItem();
    if (!historyItem)
        return nullptr;
    return historyItem->stateObject();
}

bool History::stateChanged() const
{
    return m_lastStateObjectRequested != stateInternal();
}

JSValueInWrappedObject& History::cachedState()
{
    if (m_cachedState && stateChanged())
        m_cachedState.clear();
    return m_cachedState;
}

bool History::isSameAsCurrentState(SerializedScriptValue* state) const
{
    return state == stateInternal();
}

ExceptionOr<void> History::back()
{
    return go(-1);
}

ExceptionOr<void> History::back(Document& document)
{
    return go(document, -1);
}

ExceptionOr<void> History::forward()
{
    return go(1);
}

ExceptionOr<void> History::forward(Document& document)
{
    return go(document, 1);
}

ExceptionOr<void> History::go(int distance)
{
    auto* frame = this->frame();
    LOG(History, "History %p go(%d) frame %p (main frame %d)", this, distance, frame, frame ? frame->isMainFrame() : false);

    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();

    frame->navigationScheduler().scheduleHistoryNavigation(distance);
    return { };
}

ExceptionOr<void> History::go(Document& document, int distance)
{
    auto* frame = this->frame();
    LOG(History, "History %p go(%d) in document %p frame %p (main frame %d)", this, distance, &document, frame, frame ? frame->isMainFrame() : false);

    if (!isDocumentFullyActive(frame))
        return documentNotFullyActive();

    ASSERT(isMainThread());

    if (!document.canNavigate(frame))
        return { };

    frame->navigationScheduler().scheduleHistoryNavigation(distance);
    return { };
}

URL History::urlForState(const String& urlString)
{
    auto* frame = this->frame();
    if (urlString.isNull())
        return frame->document()->url();
    return frame->document()->completeURL(urlString);
}

ExceptionOr<void> History::stateObjectAdded(RefPtr<SerializedScriptValue>&& data, const String& urlString, StateObjectType stateObjectType)
{
    m_cachedState.clear();

    // Each unique main-frame document is only allowed to send 64MB of state object payload to the UI client/process.
    static uint32_t totalStateObjectPayloadLimit = 0x4000000;
    static Seconds stateObjectTimeSpan { 10_s };
    static unsigned perStateObjectTimeSpanLimit = 100;

    RefPtr frame = this->frame();
    if (!isDocumentFullyActive(frame.get()))
        return documentNotFullyActive();
    if (!frame->page())
        return { };

    URL fullURL = urlForState(urlString);
    if (!fullURL.isValid())
        return Exception { ExceptionCode::SecurityError };

    const URL& documentURL = frame->document()->url();

    auto createBlockedURLSecurityErrorWithMessageSuffix = [&] (const char* suffix) {
        const char* functionName = stateObjectType == StateObjectType::Replace ? "history.replaceState()" : "history.pushState()";
        return Exception { ExceptionCode::SecurityError, makeString("Blocked attempt to use ", functionName, " to change session history URL from ", documentURL.stringCenterEllipsizedToLength(), " to ", fullURL.stringCenterEllipsizedToLength(), ". ", suffix) };
    };
    if (!protocolHostAndPortAreEqual(fullURL, documentURL) || fullURL.user() != documentURL.user() || fullURL.password() != documentURL.password())
        return createBlockedURLSecurityErrorWithMessageSuffix("Protocols, domains, ports, usernames, and passwords must match.");

    if (fullURL.protocolIsFile()
#if PLATFORM(COCOA)
        && linkedOnOrAfterSDKWithBehavior(SDKAlignedBehavior::PushStateFilePathRestriction)
#endif
        && !frame->document()->quirks().shouldDisablePushStateFilePathRestrictions()
        && fullURL.fileSystemPath() != documentURL.fileSystemPath()) {
        return createBlockedURLSecurityErrorWithMessageSuffix("Only differences in query and fragment are allowed for file: URLs.");
    }

    Ref documentSecurityOrigin = frame->document()->securityOrigin();
    // We allow sandboxed documents, 'data:'/'file:' URLs, etc. to use 'pushState'/'replaceState' to modify the URL query and fragments.
    // See https://bugs.webkit.org/show_bug.cgi?id=183028 for the compatibility concerns.
    bool allowSandboxException = (documentSecurityOrigin->isLocal() || documentSecurityOrigin->isOpaque())
        && documentURL.viewWithoutQueryOrFragmentIdentifier() == fullURL.viewWithoutQueryOrFragmentIdentifier();

    if (!allowSandboxException && !documentSecurityOrigin->canRequest(fullURL, OriginAccessPatternsForWebProcess::singleton()) && (fullURL.path() != documentURL.path() || fullURL.query() != documentURL.query()))
        return createBlockedURLSecurityErrorWithMessageSuffix("Paths and fragments must match for a sandboxed document.");

    RefPtr localMainFrame = dynamicDowncast<LocalFrame>(frame->page()->mainFrame());
    RefPtr mainWindow = localMainFrame ? localMainFrame->window() : nullptr;
    if (!mainWindow)
        return { };

    Ref mainHistory = mainWindow->history();

    WallTime currentTimestamp = WallTime::now();
    if (currentTimestamp - mainHistory->m_currentStateObjectTimeSpanStart > stateObjectTimeSpan) {
        mainHistory->m_currentStateObjectTimeSpanStart = currentTimestamp;
        mainHistory->m_currentStateObjectTimeSpanObjectsAdded = 0;
    }
    
    if (mainHistory->m_currentStateObjectTimeSpanObjectsAdded >= perStateObjectTimeSpanLimit) {
        if (stateObjectType == StateObjectType::Replace)
            return Exception { ExceptionCode::SecurityError, makeString("Attempt to use history.replaceState() more than ", perStateObjectTimeSpanLimit, " times per ", stateObjectTimeSpan.seconds(), " seconds") };
        return Exception { ExceptionCode::SecurityError, makeString("Attempt to use history.pushState() more than ", perStateObjectTimeSpanLimit, " times per ", stateObjectTimeSpan.seconds(), " seconds") };
    }

    Checked<unsigned> urlSize = fullURL.string().length();
    urlSize *= 2;

    Checked<uint64_t> payloadSize = urlSize;
    payloadSize += data ? data->wireBytes().size() : 0;

    Checked<uint64_t> newTotalUsage = mainHistory->m_totalStateObjectUsage;

    if (stateObjectType == StateObjectType::Replace)
        newTotalUsage -= m_mostRecentStateObjectUsage;
    newTotalUsage += payloadSize;

    if (newTotalUsage > totalStateObjectPayloadLimit) {
        if (stateObjectType == StateObjectType::Replace)
            return Exception { ExceptionCode::QuotaExceededError, "Attempt to store more data than allowed using history.replaceState()"_s };
        return Exception { ExceptionCode::QuotaExceededError, "Attempt to store more data than allowed using history.pushState()"_s };
    }

    m_mostRecentStateObjectUsage = payloadSize;

    mainHistory->m_totalStateObjectUsage = newTotalUsage;
    ++mainHistory->m_currentStateObjectTimeSpanObjectsAdded;

    if (!urlString.isEmpty())
        frame->protectedDocument()->updateURLForPushOrReplaceState(fullURL);

    if (stateObjectType == StateObjectType::Push) {
        frame->loader().history().pushState(WTFMove(data), fullURL.string());
        frame->loader().client().dispatchDidPushStateWithinPage();
    } else if (stateObjectType == StateObjectType::Replace) {
        frame->loader().history().replaceState(WTFMove(data), fullURL.string());
        frame->loader().client().dispatchDidReplaceStateWithinPage();
    }

    return { };
}

} // namespace WebCore
