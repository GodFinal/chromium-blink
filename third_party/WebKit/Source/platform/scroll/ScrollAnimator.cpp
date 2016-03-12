/*
 * Copyright (c) 2011, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "platform/scroll/ScrollAnimator.h"

#include "platform/TraceEvent.h"
#include "platform/scroll/ScrollableArea.h"
#include "public/platform/Platform.h"
#include "public/platform/WebCompositorSupport.h"
#include "wtf/CurrentTime.h"
#include "wtf/PassRefPtr.h"
#include <algorithm>

namespace blink {

PassOwnPtrWillBeRawPtr<ScrollAnimatorBase> ScrollAnimatorBase::create(ScrollableArea* scrollableArea)
{
    if (scrollableArea && scrollableArea->scrollAnimatorEnabled())
        return adoptPtrWillBeNoop(new ScrollAnimator(scrollableArea));
    return adoptPtrWillBeNoop(new ScrollAnimatorBase(scrollableArea));
}

ScrollAnimator::ScrollAnimator(ScrollableArea* scrollableArea, WTF::TimeFunction timeFunction)
    : ScrollAnimatorBase(scrollableArea)
    , m_timeFunction(timeFunction)
{
}

ScrollAnimator::~ScrollAnimator()
{
    cancelAnimations();
}

FloatPoint ScrollAnimator::desiredTargetPosition() const
{
    return m_animationCurve ? FloatPoint(m_animationCurve->targetValue()) : currentPosition();
}

float ScrollAnimator::computeDeltaToConsume(ScrollbarOrientation orientation, float pixelDelta) const
{
    FloatPoint pos = desiredTargetPosition();
    float currentPos = (orientation == HorizontalScrollbar) ? pos.x() : pos.y();
    float newPos = clampScrollPosition(orientation, currentPos + pixelDelta);
    return (currentPos == newPos) ? 0.0f : (newPos - currentPos);
}

ScrollResultOneDimensional ScrollAnimator::userScroll(ScrollbarOrientation orientation, ScrollGranularity granularity, float step, float delta)
{
    if (!m_scrollableArea->scrollAnimatorEnabled())
        return ScrollAnimatorBase::userScroll(orientation, granularity, step, delta);

    TRACE_EVENT0("blink", "ScrollAnimator::scroll");

    if (granularity == ScrollByPrecisePixel)
        return ScrollAnimatorBase::userScroll(orientation, granularity, step, delta);

    float usedPixelDelta = computeDeltaToConsume(orientation, step * delta);
    FloatPoint pixelDelta = (orientation == VerticalScrollbar ? FloatPoint(0, usedPixelDelta) : FloatPoint(usedPixelDelta, 0));

    FloatPoint targetPos = desiredTargetPosition();
    targetPos.moveBy(pixelDelta);

    if (m_animationCurve) {
        if (!(targetPos - m_animationCurve->targetValue()).isZero())
            m_animationCurve->updateTarget(m_timeFunction() - m_startTime, targetPos);
        return ScrollResultOneDimensional(/* didScroll */ true, /* unusedScrollDelta */ 0);
    }

    if ((targetPos - currentPosition()).isZero()) {
        // Report unused delta only if there is no animation and we are not
        // starting one. This ensures we latch for the duration of the
        // animation rather than animating multiple scrollers at the same time.
        return ScrollResultOneDimensional(/* didScroll */ false, delta);
    }

    m_animationCurve = adoptPtr(Platform::current()->compositorSupport()->createScrollOffsetAnimationCurve(
        targetPos,
        WebCompositorAnimationCurve::TimingFunctionTypeEaseInOut,
        WebScrollOffsetAnimationCurve::ScrollDurationConstant));

    m_animationCurve->setInitialValue(currentPosition());
    m_startTime = m_timeFunction();

    scrollableArea()->registerForAnimation();
    animationTimerFired();
    return ScrollResultOneDimensional(/* didScroll */ true, /* unusedScrollDelta */ 0);
}

void ScrollAnimator::scrollToOffsetWithoutAnimation(const FloatPoint& offset)
{
    m_currentPosX = offset.x();
    m_currentPosY = offset.y();

    cancelAnimations();
    notifyPositionChanged();
}

void ScrollAnimator::cancelAnimations()
{
    if (m_animationCurve)
        m_animationCurve.clear();
}

void ScrollAnimator::serviceScrollAnimations()
{
    if (hasRunningAnimation())
        animationTimerFired();
}

bool ScrollAnimator::hasRunningAnimation() const
{
    return m_animationCurve;
}

void ScrollAnimator::animationTimerFired()
{
    TRACE_EVENT0("blink", "ScrollAnimator::animationTimerFired");
    double elapsedTime = m_timeFunction() - m_startTime;

    bool isFinished = (elapsedTime > m_animationCurve->duration());
    FloatPoint offset = isFinished ? m_animationCurve->targetValue() : m_animationCurve->getValue(elapsedTime);

    offset = FloatPoint(m_scrollableArea->clampScrollPosition(offset));

    m_currentPosX = offset.x();
    m_currentPosY = offset.y();

    if (isFinished)
        m_animationCurve.clear();
    else
        scrollableArea()->scheduleAnimation();

    TRACE_EVENT0("blink", "ScrollAnimator::notifyPositionChanged");
    notifyPositionChanged();
}

DEFINE_TRACE(ScrollAnimator)
{
    ScrollAnimatorBase::trace(visitor);
}

} // namespace blink