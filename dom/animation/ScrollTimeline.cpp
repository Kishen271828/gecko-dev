/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScrollTimeline.h"

#include "mozilla/dom/Animation.h"
#include "mozilla/dom/ElementInlines.h"
#include "mozilla/AnimationTarget.h"
#include "mozilla/DisplayPortUtils.h"
#include "mozilla/PresShell.h"
#include "nsIFrame.h"
#include "nsIScrollableFrame.h"
#include "nsLayoutUtils.h"

namespace mozilla::dom {

// ---------------------------------
// Methods of ScrollTimeline
// ---------------------------------

NS_IMPL_CYCLE_COLLECTION_CLASS(ScrollTimeline)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(ScrollTimeline,
                                                AnimationTimeline)
  tmp->Teardown();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDocument)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSource.mElement)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ScrollTimeline,
                                                  AnimationTimeline)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDocument)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSource.mElement)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(ScrollTimeline,
                                               AnimationTimeline)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(ScrollTimeline,
                                               AnimationTimeline)

ScrollTimeline::ScrollTimeline(Document* aDocument, const Scroller& aScroller,
                               StyleScrollAxis aAxis)
    : AnimationTimeline(aDocument->GetParentObject(),
                        aDocument->GetScopeObject()->GetRTPCallerType()),
      mDocument(aDocument),
      mSource(aScroller),
      mAxis(aAxis) {
  MOZ_ASSERT(aDocument);
  RegisterWithScrollSource();
}

static Element* FindNearestScroller(const Element* aSubject) {
  MOZ_ASSERT(aSubject);
  Element* curr = aSubject->GetFlattenedTreeParentElement();
  Element* root = aSubject->OwnerDoc()->GetDocumentElement();
  while (curr && curr != root) {
    const ComputedStyle* style = Servo_Element_GetMaybeOutOfDateStyle(curr);
    MOZ_ASSERT(style, "The ancestor should be styled.");
    if (style->StyleDisplay()->IsScrollableOverflow()) {
      break;
    }
    curr = curr->GetFlattenedTreeParentElement();
  }
  // If there is no scroll container, we use root.
  return curr ? curr : root;
}

/* static */
already_AddRefed<ScrollTimeline> ScrollTimeline::MakeAnonymous(
    Document* aDocument, const NonOwningAnimationTarget& aTarget,
    StyleScrollAxis aAxis, StyleScroller aScroller) {
  MOZ_ASSERT(aTarget);
  Scroller scroller;
  switch (aScroller) {
    case StyleScroller::Root:
      scroller = Scroller::Root(aTarget.mElement->OwnerDoc());
      break;

    case StyleScroller::Nearest: {
      scroller = Scroller::Nearest(FindNearestScroller(aTarget.mElement));
      break;
    }
  }

  // Note: We create new ScrollTimeline for anonymous scroll timeline, i.e.
  // scroll(). In other words, each anonymous scroll timeline is a different
  // object per the resolution of this spec issue:
  // https://github.com/w3c/csswg-drafts/issues/8204
  //
  // FIXME: Perhaps it's still possible to reuse scroll(root). Need to revisit
  // this after we start to work on JS support.
  return MakeAndAddRef<ScrollTimeline>(aDocument, scroller, aAxis);
}

/* static*/ already_AddRefed<ScrollTimeline> ScrollTimeline::MakeNamed(
    Document* aDocument, Element* aReferenceElement,
    const StyleScrollTimeline& aStyleTimeline) {
  MOZ_ASSERT(NS_IsMainThread());

  Scroller scroller = Scroller::Named(aReferenceElement);
  return MakeAndAddRef<ScrollTimeline>(aDocument, std::move(scroller),
                                       aStyleTimeline.GetAxis());
}

Nullable<TimeDuration> ScrollTimeline::GetCurrentTimeAsDuration() const {
  // If no layout box, this timeline is inactive.
  if (!mSource || !mSource.mElement->GetPrimaryFrame()) {
    return nullptr;
  }

  // if this is not a scroller container, this timeline is inactive.
  const nsIScrollableFrame* scrollFrame = GetScrollFrame();
  if (!scrollFrame) {
    return nullptr;
  }

  const auto orientation = Axis();

  // If there is no scrollable overflow, then the ScrollTimeline is inactive.
  // https://drafts.csswg.org/scroll-animations-1/#scrolltimeline-interface
  if (!scrollFrame->GetAvailableScrollingDirections().contains(orientation)) {
    return nullptr;
  }

  const nsPoint& scrollOffset = scrollFrame->GetScrollPosition();
  const nsRect& scrollRange = scrollFrame->GetScrollRange();
  const bool isHorizontal = orientation == layers::ScrollDirection::eHorizontal;

  // Note: For RTL, scrollOffset.x or scrollOffset.y may be negative, e.g. the
  // range of its value is [0, -range], so we have to use the absolute value.
  double position = std::abs(isHorizontal ? scrollOffset.x : scrollOffset.y);
  double range = isHorizontal ? scrollRange.width : scrollRange.height;
  MOZ_ASSERT(range > 0.0);
  // Use the definition of interval progress to compute the progress.
  // Note: We simplify the scroll offsets to [0%, 100%], so offset weight and
  // offset index are ignored here.
  // https://drafts.csswg.org/scroll-animations-1/#progress-calculation-algorithm
  double progress = position / range;
  return TimeDuration::FromMilliseconds(progress *
                                        PROGRESS_TIMELINE_DURATION_MILLISEC);
}

layers::ScrollDirection ScrollTimeline::Axis() const {
  MOZ_ASSERT(mSource && mSource.mElement->GetPrimaryFrame());

  const WritingMode wm = mSource.mElement->GetPrimaryFrame()->GetWritingMode();
  return mAxis == StyleScrollAxis::Horizontal ||
                 (!wm.IsVertical() && mAxis == StyleScrollAxis::Inline) ||
                 (wm.IsVertical() && mAxis == StyleScrollAxis::Block)
             ? layers::ScrollDirection::eHorizontal
             : layers::ScrollDirection::eVertical;
}

StyleOverflow ScrollTimeline::SourceScrollStyle() const {
  MOZ_ASSERT(mSource && mSource.mElement->GetPrimaryFrame());

  const nsIScrollableFrame* scrollFrame = GetScrollFrame();
  MOZ_ASSERT(scrollFrame);

  const ScrollStyles scrollStyles = scrollFrame->GetScrollStyles();

  return Axis() == layers::ScrollDirection::eHorizontal
             ? scrollStyles.mHorizontal
             : scrollStyles.mVertical;
}

bool ScrollTimeline::APZIsActiveForSource() const {
  MOZ_ASSERT(mSource);
  return gfxPlatform::AsyncPanZoomEnabled() &&
         !nsLayoutUtils::ShouldDisableApzForElement(mSource.mElement) &&
         DisplayPortUtils::HasNonMinimalNonZeroDisplayPort(mSource.mElement);
}

bool ScrollTimeline::ScrollingDirectionIsAvailable() const {
  const nsIScrollableFrame* scrollFrame = GetScrollFrame();
  MOZ_ASSERT(scrollFrame);
  return scrollFrame->GetAvailableScrollingDirections().contains(Axis());
}

void ScrollTimeline::ReplacePropertiesWith(const Element* aReferenceElement,
                                           const StyleScrollTimeline& aNew) {
  MOZ_ASSERT(aReferenceElement == mSource.mElement);
  mAxis = aNew.GetAxis();

  for (auto* anim = mAnimationOrder.getFirst(); anim;
       anim = static_cast<LinkedListElement<Animation>*>(anim)->getNext()) {
    MOZ_ASSERT(anim->GetTimeline() == this);
    // Set this so we just PostUpdate() for this animation.
    anim->SetTimeline(this);
  }
}

void ScrollTimeline::RegisterWithScrollSource() {
  if (!mSource) {
    return;
  }

  if (ScrollTimelineSet* scrollTimelineSet =
          ScrollTimelineSet::GetOrCreateScrollTimelineSet(mSource.mElement)) {
    scrollTimelineSet->AddScrollTimeline(this);
  }
}

void ScrollTimeline::UnregisterFromScrollSource() {
  if (!mSource) {
    return;
  }

  if (ScrollTimelineSet* scrollTimelineSet =
          ScrollTimelineSet::GetScrollTimelineSet(mSource.mElement)) {
    scrollTimelineSet->RemoveScrollTimeline(this);
    if (scrollTimelineSet->IsEmpty()) {
      ScrollTimelineSet::DestroyScrollTimelineSet(mSource.mElement);
    }
  }
}

const nsIScrollableFrame* ScrollTimeline::GetScrollFrame() const {
  if (!mSource) {
    return nullptr;
  }

  switch (mSource.mType) {
    case Scroller::Type::Root:
      if (const PresShell* presShell =
              mSource.mElement->OwnerDoc()->GetPresShell()) {
        return presShell->GetRootScrollFrameAsScrollable();
      }
      return nullptr;
    case Scroller::Type::Nearest:
    case Scroller::Type::Name:
      return nsLayoutUtils::FindScrollableFrameFor(mSource.mElement);
  }

  MOZ_ASSERT_UNREACHABLE("Unsupported scroller type");
  return nullptr;
}

// ---------------------------------
// Methods of ScrollTimelineSet
// ---------------------------------

/* static */ ScrollTimelineSet* ScrollTimelineSet::GetScrollTimelineSet(
    Element* aElement) {
  return aElement ? static_cast<ScrollTimelineSet*>(aElement->GetProperty(
                        nsGkAtoms::scrollTimelinesProperty))
                  : nullptr;
}

/* static */ ScrollTimelineSet* ScrollTimelineSet::GetOrCreateScrollTimelineSet(
    Element* aElement) {
  MOZ_ASSERT(aElement);
  ScrollTimelineSet* scrollTimelineSet = GetScrollTimelineSet(aElement);
  if (scrollTimelineSet) {
    return scrollTimelineSet;
  }

  scrollTimelineSet = new ScrollTimelineSet();
  nsresult rv = aElement->SetProperty(
      nsGkAtoms::scrollTimelinesProperty, scrollTimelineSet,
      nsINode::DeleteProperty<ScrollTimelineSet>, true);
  if (NS_FAILED(rv)) {
    NS_WARNING("SetProperty failed");
    delete scrollTimelineSet;
    return nullptr;
  }
  return scrollTimelineSet;
}

/* static */ void ScrollTimelineSet::DestroyScrollTimelineSet(
    Element* aElement) {
  aElement->RemoveProperty(nsGkAtoms::scrollTimelinesProperty);
}

}  // namespace mozilla::dom
