// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineBodyTypes.h"
#include "CoreMinimal.h"

/**
 * Rule-based description → FCineBodySpec parser (the offline provider). A
 * Claude-backed provider can later replace this behind the same spec, exactly
 * like IShotPlanProvider replaces FShotGrammarParser for shots.
 */
class FCineBodyGrammar
{
public:
	static FCineBodySpec Parse(const FString& Description, int32 Seed);

	/** Help text for the panel: what words the parser understands. */
	static FText GetVocabularyHelpText();
};
