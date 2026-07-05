// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IShotPlanProvider.h"

/**
 * Rule-based IShotPlanProvider. Recognizes a cinematography vocabulary:
 *
 *  Moves:    orbit/circle around, dolly/push in, pull back, truck/track left|right,
 *            crane/boom/pedestal up|down, pan left|right, tilt up|down, zoom in|out,
 *            flyover/drone/aerial, static/locked
 *  Framing:  extreme close-up, close-up, medium close-up, medium, wide, extreme wide/establishing
 *  Angles:   low angle, high angle, overhead/top-down/bird's eye, eye level
 *  Side:     from behind, from the front, from the left/right, over the shoulder
 *  Lens:     "50mm", wide-angle, portrait, telephoto; f/1.8, shallow focus, deep focus
 *  Focus:    focus on <actor>, rack focus from <actor> to <actor>, tracking focus
 *  Effects:  handheld/shaky (slightly/very), dutch/canted angle
 *  Timing:   "over 8 seconds", "for 3s", slow/fast modifiers
 *  Amounts:  "180 degrees", half/full orbit, "by 5 meters"
 *  Multi:    clauses split on ".", ";", newlines, "then", "cut to", "next", "after that"
 *
 * Target actors are resolved by fuzzy-matching actor labels from the level against the text.
 */
class FShotGrammarParser : public IShotPlanProvider
{
public:
	virtual FText GetProviderName() const override;
	virtual bool BuildShotPlan(const FString& Description, const FCineSceneContext& Scene, FCineShotPlan& OutPlan, FText& OutError) override;

	/** Human-readable cheat sheet of the grammar, shown in the panel's help expander. */
	static FText GetVocabularyHelpText();

private:
	/** Split a description into per-shot clauses. */
	static TArray<FString> SplitIntoSegments(const FString& Description);

	/** Parse a single clause into a segment. Returns false if the clause contained nothing usable. */
	static bool ParseSegment(const FString& Clause, const FCineSceneContext& Scene, FCineShotSegment& OutSegment);

	/** Find the level actor whose label best matches somewhere in the clause. */
	static const FCineSceneActorInfo* ResolveTarget(const FString& LowerClause, const FCineSceneContext& Scene, const FString* ExcludeLabel = nullptr);
};
