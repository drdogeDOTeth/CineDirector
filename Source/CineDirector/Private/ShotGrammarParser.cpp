// Copyright Roundtree. All Rights Reserved.

#include "ShotGrammarParser.h"

#include "Internationalization/Regex.h"

#define LOCTEXT_NAMESPACE "CineDirector"

namespace CineDirectorGrammar
{
	/** True if Phrase occurs in Text on word boundaries. Both must already be lowercase. */
	bool ContainsPhrase(const FString& Text, const TCHAR* Phrase)
	{
		const int32 PhraseLen = FCString::Strlen(Phrase);
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 Idx = Text.Find(Phrase, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Idx == INDEX_NONE)
			{
				return false;
			}
			const bool bStartOk = (Idx == 0) || !FChar::IsAlnum(Text[Idx - 1]);
			const int32 End = Idx + PhraseLen;
			const bool bEndOk = (End >= Text.Len()) || !FChar::IsAlnum(Text[End]);
			if (bStartOk && bEndOk)
			{
				return true;
			}
			SearchFrom = Idx + 1;
		}
	}

	bool ContainsAny(const FString& Text, std::initializer_list<const TCHAR*> Phrases)
	{
		for (const TCHAR* Phrase : Phrases)
		{
			if (ContainsPhrase(Text, Phrase))
			{
				return true;
			}
		}
		return false;
	}

	/** Collapse runs of whitespace to single spaces and trim the ends. */
	void CollapseAndTrim(FString& S)
	{
		FString Out;
		Out.Reserve(S.Len());
		bool bPrevSpace = true;
		for (const TCHAR C : S)
		{
			if (FChar::IsWhitespace(C))
			{
				if (!bPrevSpace)
				{
					Out.AppendChar(TEXT(' '));
					bPrevSpace = true;
				}
			}
			else
			{
				Out.AppendChar(C);
				bPrevSpace = false;
			}
		}
		Out.TrimEndInline();
		S = MoveTemp(Out);
	}

	void AddCandidate(TArray<FString>& Out, FString S, int32 MinLen = 3)
	{
		CollapseAndTrim(S);
		if (S.Len() >= MinLen)
		{
			Out.AddUnique(S);
		}
	}

	/**
	 * All the spellings of an actor label a user might plausibly type, longest first
	 * wins via scoring. "void_4003GasMask" yields: "void 4003gasmask",
	 * "void 4003 gas mask", "void gas mask", "gas mask", "gasmask", ...
	 */
	TArray<FString> BuildLabelCandidates(const FString& Label)
	{
		TArray<FString> Candidates;

		auto NormalizeSeparators = [](FString& S)
		{
			S.ReplaceInline(TEXT("_"), TEXT(" "));
			S.ReplaceInline(TEXT("-"), TEXT(" "));
			S.ReplaceInline(TEXT("."), TEXT(" "));
		};

		// The label as written.
		FString Normalized = Label.ToLower();
		NormalizeSeparators(Normalized);
		AddCandidate(Candidates, Normalized);

		// Camel-case and letter/digit boundaries split into words.
		FString Camel;
		Camel.Reserve(Label.Len() + 8);
		for (int32 i = 0; i < Label.Len(); ++i)
		{
			const TCHAR C = Label[i];
			if (i > 0)
			{
				const TCHAR P = Label[i - 1];
				const bool bBoundary =
					(FChar::IsUpper(C) && FChar::IsLower(P)) ||
					(FChar::IsAlnum(C) && FChar::IsAlnum(P) && FChar::IsDigit(C) != FChar::IsDigit(P));
				if (bBoundary)
				{
					Camel.AppendChar(TEXT(' '));
				}
			}
			Camel.AppendChar(C);
		}
		Camel = Camel.ToLower();
		NormalizeSeparators(Camel);
		AddCandidate(Candidates, Camel);

		// Without trailing digits, so "Knight_2" is found by "the knight".
		const int32 NumBase = Candidates.Num();
		for (int32 i = 0; i < NumBase; ++i)
		{
			FString Stripped = Candidates[i];
			while (Stripped.Len() > 0 && (FChar::IsDigit(Stripped[Stripped.Len() - 1]) || FChar::IsWhitespace(Stripped[Stripped.Len() - 1])))
			{
				Stripped.LeftChopInline(1);
			}
			AddCandidate(Candidates, Stripped);
		}

		// Without digit words anywhere ("void 4003 gas mask" -> "void gas mask"),
		// plus word suffixes so "the gas mask" finds "void_4003GasMask". Suffixes
		// only (never prefixes): the trailing words are usually the noun, while
		// leading words are pack prefixes like "SM" that would misfire.
		TArray<FString> Words;
		Camel.ParseIntoArray(Words, TEXT(" "));
		Words.RemoveAll([](const FString& W)
		{
			for (const TCHAR C : W)
			{
				if (!FChar::IsDigit(C))
				{
					return false;
				}
			}
			return true;
		});
		if (Words.Num() > 0)
		{
			AddCandidate(Candidates, FString::Join(Words, TEXT(" ")));
			for (int32 First = 1; First < Words.Num(); ++First)
			{
				const TArray<FString> Suffix(Words.GetData() + First, Words.Num() - First);
				AddCandidate(Candidates, FString::Join(Suffix, TEXT(" ")), Suffix.Num() == 1 ? 4 : 3);
			}
		}

		// Single words with leading digits stripped ("4003gasmask" -> "gasmask").
		TArray<FString> RawWords;
		Normalized.ParseIntoArray(RawWords, TEXT(" "));
		for (FString Word : RawWords)
		{
			const int32 OriginalLen = Word.Len();
			while (Word.Len() > 0 && FChar::IsDigit(Word[0]))
			{
				Word.RemoveAt(0, 1, EAllowShrinking::No);
			}
			if (Word.Len() < OriginalLen)
			{
				AddCandidate(Candidates, Word, 4);
			}
		}

		// Condensed variants ("x logo" -> "xlogo") so a label typed without its
		// separators still matches.
		const int32 NumSpaced = Candidates.Num();
		for (int32 i = 0; i < NumSpaced; ++i)
		{
			AddCandidate(Candidates, Candidates[i].Replace(TEXT(" "), TEXT("")), 4);
		}

		return Candidates;
	}

	/** First numeric capture of Pattern in Text, or 0 if absent. */
	bool MatchNumber(const FString& Text, const TCHAR* Pattern, double& OutValue)
	{
		FRegexPattern Regex(Pattern);
		FRegexMatcher Matcher(Regex, Text);
		if (Matcher.FindNext())
		{
			OutValue = FCString::Atod(*Matcher.GetCaptureGroup(1));
			return true;
		}
		return false;
	}
}

FText FShotGrammarParser::GetProviderName() const
{
	return LOCTEXT("RuleParserName", "Rule-Based Parser (Built-in)");
}

TArray<FString> FShotGrammarParser::SplitIntoSegments(const FString& Description)
{
	FString Work = Description.ToLower();

	// Normalize the phrase separators into sentence breaks first.
	const TCHAR* PhraseBreaks[] = { TEXT(" then "), TEXT(" cut to "), TEXT(" next, "), TEXT(" after that "), TEXT(" followed by ") };
	for (const TCHAR* Break : PhraseBreaks)
	{
		Work.ReplaceInline(Break, TEXT(";"));
	}

	// Split on '.', ';', '\n' — but never inside a decimal number like "3.5 seconds".
	TArray<FString> Segments;
	FString Current;
	for (int32 i = 0; i < Work.Len(); ++i)
	{
		const TCHAR C = Work[i];
		const bool bIsBreak =
			(C == TEXT(';') || C == TEXT('\n') || C == TEXT('\r')) ||
			(C == TEXT('.') &&
				!(i > 0 && FChar::IsDigit(Work[i - 1]) && i + 1 < Work.Len() && FChar::IsDigit(Work[i + 1])));

		if (bIsBreak)
		{
			Segments.Add(Current);
			Current.Reset();
		}
		else
		{
			Current.AppendChar(C);
		}
	}
	Segments.Add(Current);

	// Drop clauses with nothing in them.
	Segments.RemoveAll([](const FString& S)
	{
		FString Trimmed = S.TrimStartAndEnd();
		return Trimmed.Len() < 3;
	});
	return Segments;
}

const FCineSceneActorInfo* FShotGrammarParser::ResolveTarget(const FString& LowerClause, const FCineSceneContext& Scene, const FString* ExcludeLabel)
{
	using namespace CineDirectorGrammar;

	// The user may type a label with its underscores/hyphens intact
	// ("void_4003GasMask"), so match against a separator-normalized copy of the
	// clause — labels are normalized the same way in BuildLabelCandidates.
	FString NormClause = LowerClause;
	NormClause.ReplaceInline(TEXT("_"), TEXT(" "));
	NormClause.ReplaceInline(TEXT("-"), TEXT(" "));
	NormClause.ReplaceInline(TEXT("."), TEXT(" "));
	CollapseAndTrim(NormClause);

	const FCineSceneActorInfo* Best = nullptr;
	int32 BestScore = 0;

	for (const FCineSceneActorInfo& Info : Scene.Actors)
	{
		if (!Info.Actor.IsValid())
		{
			continue;
		}
		if (ExcludeLabel && Info.Label == *ExcludeLabel)
		{
			continue;
		}

		for (const FString& Candidate : BuildLabelCandidates(Info.Label))
		{
			if (Candidate.Len() > BestScore && ContainsPhrase(NormClause, *Candidate))
			{
				Best = &Info;
				BestScore = Candidate.Len();
			}
		}
	}
	return Best;
}

bool FShotGrammarParser::ParseSegment(const FString& Clause, const FCineSceneContext& Scene, FCineShotSegment& OutSegment)
{
	using namespace CineDirectorGrammar;

	const FString Text = Clause.TrimStartAndEnd();
	OutSegment.RawText = Text;

	bool bRecognizedAnything = false;

	// ---- Target actor -------------------------------------------------------
	if (const FCineSceneActorInfo* Target = ResolveTarget(Text, Scene))
	{
		OutSegment.TargetActor = Target->Actor;
		OutSegment.TargetLabel = Target->Label;
		bRecognizedAnything = true;
	}

	// ---- Lens (before framing: "wide-angle" must not read as a wide shot) ---
	double Number = 0.0;
	bool bWideAngleLens = ContainsAny(Text, { TEXT("wide angle"), TEXT("wide-angle") });
	if (MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*mm\\b"), Number))
	{
		OutSegment.FocalLengthMm = static_cast<float>(Number);
		bRecognizedAnything = true;
	}
	else if (bWideAngleLens)
	{
		OutSegment.FocalLengthMm = 18.0f;
		bRecognizedAnything = true;
	}
	else if (ContainsPhrase(Text, TEXT("telephoto")))
	{
		OutSegment.FocalLengthMm = 135.0f;
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("portrait lens"), TEXT("portrait") }))
	{
		OutSegment.FocalLengthMm = 85.0f;
	}

	if (MatchNumber(Text, TEXT("f\\s*/?\\s*(\\d+(?:\\.\\d+)?)"), Number))
	{
		OutSegment.Aperture = static_cast<float>(Number);
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("shallow focus"), TEXT("shallow depth"), TEXT("shallow dof"), TEXT("blurry background"), TEXT("bokeh") }))
	{
		OutSegment.Aperture = 1.4f;
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("deep focus"), TEXT("deep depth"), TEXT("everything in focus") }))
	{
		OutSegment.Aperture = 11.0f;
		bRecognizedAnything = true;
	}

	// ---- Framing -------------------------------------------------------------
	if (ContainsAny(Text, { TEXT("extreme close"), TEXT("extreme close-up"), TEXT("ecu") }))
	{
		OutSegment.ShotSize = ECineShotSize::ExtremeCloseUp;
	}
	else if (ContainsAny(Text, { TEXT("medium close"), TEXT("medium close-up") }))
	{
		OutSegment.ShotSize = ECineShotSize::MediumCloseUp;
	}
	else if (ContainsAny(Text, { TEXT("close-up"), TEXT("close up"), TEXT("closeup") }))
	{
		OutSegment.ShotSize = ECineShotSize::CloseUp;
	}
	else if (ContainsAny(Text, { TEXT("extreme wide"), TEXT("very wide"), TEXT("establishing") }))
	{
		OutSegment.ShotSize = ECineShotSize::ExtremeWide;
	}
	else if (!bWideAngleLens && ContainsAny(Text, { TEXT("wide shot"), TEXT("wide") }))
	{
		OutSegment.ShotSize = ECineShotSize::Wide;
	}
	else if (ContainsAny(Text, { TEXT("medium shot"), TEXT("medium"), TEXT("mid shot"), TEXT("mid-shot") }))
	{
		OutSegment.ShotSize = ECineShotSize::Medium;
	}
	bRecognizedAnything |= (OutSegment.ShotSize != ECineShotSize::Unspecified);

	// ---- Angle ----------------------------------------------------------------
	if (ContainsAny(Text, { TEXT("overhead"), TEXT("top-down"), TEXT("top down"), TEXT("bird's eye"), TEXT("birds eye"), TEXT("directly above") }))
	{
		OutSegment.Angle = ECineAngle::Overhead;
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("low angle"), TEXT("low-angle"), TEXT("from below"), TEXT("looking up") }))
	{
		OutSegment.Angle = ECineAngle::Low;
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("high angle"), TEXT("high-angle"), TEXT("from above"), TEXT("looking down") }))
	{
		OutSegment.Angle = ECineAngle::High;
		bRecognizedAnything = true;
	}

	// ---- View side --------------------------------------------------------------
	if (ContainsAny(Text, { TEXT("over the shoulder"), TEXT("over-the-shoulder"), TEXT("ots") }))
	{
		OutSegment.ViewSide = ECineViewSide::OverShoulder;
	}
	else if (ContainsAny(Text, { TEXT("from behind"), TEXT("behind") }))
	{
		OutSegment.ViewSide = ECineViewSide::Behind;
	}
	else if (ContainsAny(Text, { TEXT("from the left"), TEXT("from left") }))
	{
		OutSegment.ViewSide = ECineViewSide::Left;
	}
	else if (ContainsAny(Text, { TEXT("from the right"), TEXT("from right") }))
	{
		OutSegment.ViewSide = ECineViewSide::Right;
	}

	// ---- Move -------------------------------------------------------------------
	// Zoom is checked before the dolly synonyms so "zoom in" never reads as a dolly.
	if (ContainsAny(Text, { TEXT("zoom in"), TEXT("zoom into"), TEXT("zooming in") }))
	{
		OutSegment.Move = ECineMoveType::ZoomIn;
	}
	else if (ContainsAny(Text, { TEXT("zoom out"), TEXT("zooming out") }))
	{
		OutSegment.Move = ECineMoveType::ZoomOut;
	}
	else if (ContainsAny(Text, { TEXT("orbit"), TEXT("circle around"), TEXT("circle"), TEXT("rotate around"), TEXT("revolve"), TEXT("arc around"), TEXT("arc") }))
	{
		const bool bCCW = ContainsAny(Text, { TEXT("counter-clockwise"), TEXT("counterclockwise"), TEXT("counter clockwise"), TEXT("anticlockwise"), TEXT("anti-clockwise"), TEXT("ccw") });
		OutSegment.Move = bCCW ? ECineMoveType::OrbitCCW : ECineMoveType::OrbitCW;
	}
	else if (ContainsAny(Text, { TEXT("flyover"), TEXT("fly over"), TEXT("fly-over"), TEXT("drone shot"), TEXT("drone"), TEXT("aerial"), TEXT("fly past"), TEXT("flyby"), TEXT("fly by") }))
	{
		OutSegment.Move = ECineMoveType::Flyover;
	}
	else if (ContainsAny(Text, { TEXT("dolly in"), TEXT("push in"), TEXT("push-in"), TEXT("move in"), TEXT("move closer"), TEXT("moves closer"), TEXT("approach"), TEXT("push towards"), TEXT("move towards"), TEXT("dolly towards"), TEXT("dolly toward") }))
	{
		OutSegment.Move = ECineMoveType::DollyIn;
	}
	else if (ContainsAny(Text, { TEXT("dolly out"), TEXT("pull out"), TEXT("pull back"), TEXT("pull-back"), TEXT("pull away"), TEXT("move away"), TEXT("moves away"), TEXT("retreat"), TEXT("back away") }))
	{
		OutSegment.Move = ECineMoveType::DollyOut;
	}
	else if (ContainsAny(Text, { TEXT("crane up"), TEXT("boom up"), TEXT("pedestal up"), TEXT("rise"), TEXT("rises"), TEXT("ascend"), TEXT("craning up") }))
	{
		OutSegment.Move = ECineMoveType::CraneUp;
	}
	else if (ContainsAny(Text, { TEXT("crane down"), TEXT("boom down"), TEXT("pedestal down"), TEXT("descend"), TEXT("lower down"), TEXT("sink down") }))
	{
		OutSegment.Move = ECineMoveType::CraneDown;
	}
	else if (ContainsAny(Text, { TEXT("truck left"), TEXT("track left"), TEXT("slide left"), TEXT("strafe left"), TEXT("move left") }))
	{
		OutSegment.Move = ECineMoveType::TruckLeft;
	}
	else if (ContainsAny(Text, { TEXT("truck right"), TEXT("track right"), TEXT("slide right"), TEXT("strafe right"), TEXT("move right") }))
	{
		OutSegment.Move = ECineMoveType::TruckRight;
	}
	else if (ContainsAny(Text, { TEXT("pan left"), TEXT("pans left") }))
	{
		OutSegment.Move = ECineMoveType::PanLeft;
	}
	else if (ContainsAny(Text, { TEXT("pan right"), TEXT("pans right"), TEXT("pan") }))
	{
		OutSegment.Move = ECineMoveType::PanRight;
	}
	else if (ContainsAny(Text, { TEXT("tilt up"), TEXT("tilts up") }))
	{
		OutSegment.Move = ECineMoveType::TiltUp;
	}
	else if (ContainsAny(Text, { TEXT("tilt down"), TEXT("tilts down"), TEXT("tilt") }))
	{
		OutSegment.Move = ECineMoveType::TiltDown;
	}
	else if (ContainsAny(Text, { TEXT("static"), TEXT("locked"), TEXT("locked-off"), TEXT("still"), TEXT("hold"), TEXT("stationary") }))
	{
		OutSegment.Move = ECineMoveType::Static;
	}
	else
	{
		OutSegment.Move = ECineMoveType::Static;
		if (bRecognizedAnything)
		{
			OutSegment.ParseNotes.Add(TEXT("No camera move recognized — using a static shot."));
		}
	}
	if (OutSegment.Move != ECineMoveType::Static)
	{
		bRecognizedAnything = true;
	}

	// ---- Move amount ---------------------------------------------------------
	const bool bIsOrbit = OutSegment.Move == ECineMoveType::OrbitCW || OutSegment.Move == ECineMoveType::OrbitCCW;
	if (bIsOrbit || OutSegment.Move == ECineMoveType::PanLeft || OutSegment.Move == ECineMoveType::PanRight ||
		OutSegment.Move == ECineMoveType::TiltUp || OutSegment.Move == ECineMoveType::TiltDown)
	{
		if (MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*(?:°|deg\\b|degree|degrees)"), Number))
		{
			OutSegment.MoveAmount = Number;
		}
		else if (ContainsAny(Text, { TEXT("full orbit"), TEXT("full circle"), TEXT("all the way around"), TEXT("360") }))
		{
			OutSegment.MoveAmount = 360.0;
		}
		else if (ContainsAny(Text, { TEXT("half orbit"), TEXT("half circle"), TEXT("halfway around"), TEXT("180") }))
		{
			OutSegment.MoveAmount = 180.0;
		}
		else if (ContainsAny(Text, { TEXT("quarter"), TEXT("90") }))
		{
			OutSegment.MoveAmount = 90.0;
		}
	}
	else if (OutSegment.Move == ECineMoveType::ZoomIn || OutSegment.Move == ECineMoveType::ZoomOut)
	{
		// "zoom to 85mm" — the mm number becomes the zoom target rather than the starting lens.
		if (OutSegment.FocalLengthMm > 0.0f && ContainsAny(Text, { TEXT("zoom to"), TEXT("zoom into") }))
		{
			OutSegment.MoveAmount = OutSegment.FocalLengthMm;
			OutSegment.FocalLengthMm = 0.0f;
		}
	}
	else
	{
		// Linear moves: accept meters or centimeters/units.
		if (MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*(?:meters|meter|metres|metre|m)\\b"), Number) &&
			!MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*mm\\b"), Number))
		{
			MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*(?:meters|meter|metres|metre|m)\\b"), Number);
			OutSegment.MoveAmount = Number * 100.0; // meters → cm
		}
		else if (MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*(?:cm|centimeters|centimetres|units)\\b"), Number))
		{
			OutSegment.MoveAmount = Number;
		}
	}

	// ---- Focus -----------------------------------------------------------------
	const bool bMentionsRack = ContainsAny(Text, { TEXT("rack focus"), TEXT("pull focus"), TEXT("focus pull") });
	if (bMentionsRack)
	{
		// "rack focus from the knight to the door"
		const int32 FromIdx = Text.Find(TEXT(" from "));
		const int32 ToIdx = Text.Find(TEXT(" to "), ESearchCase::CaseSensitive, ESearchDir::FromStart, FMath::Max(FromIdx, 0));
		if (FromIdx != INDEX_NONE && ToIdx != INDEX_NONE && ToIdx > FromIdx)
		{
			const FString FromPart = Text.Mid(FromIdx, ToIdx - FromIdx);
			const FString ToPart = Text.Mid(ToIdx);
			if (const FCineSceneActorInfo* FromActor = ResolveTarget(FromPart, Scene))
			{
				OutSegment.TargetActor = FromActor->Actor;
				OutSegment.TargetLabel = FromActor->Label;
			}
			if (const FCineSceneActorInfo* ToActor = ResolveTarget(ToPart, Scene, &OutSegment.TargetLabel))
			{
				OutSegment.RackFocusToActor = ToActor->Actor;
				OutSegment.RackFocusToLabel = ToActor->Label;
			}
		}
		if (!OutSegment.RackFocusToActor.IsValid())
		{
			OutSegment.ParseNotes.Add(TEXT("Rack focus requested but couldn't resolve both actors (\"rack focus from <actor> to <actor>\")."));
		}
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("focus on"), TEXT("track focus"), TEXT("tracking focus"), TEXT("follow focus"), TEXT("keep focus"), TEXT("stay focused") }))
	{
		OutSegment.bTrackFocus = true;
		bRecognizedAnything = true;
	}
	else if (OutSegment.TargetActor.IsValid() && OutSegment.Aperture > 0.0f && OutSegment.Aperture <= 2.8f)
	{
		// Shallow depth of field on a named subject: keep autofocus glued to them.
		OutSegment.bTrackFocus = true;
	}

	// ---- Effects ------------------------------------------------------------------
	if (ContainsAny(Text, { TEXT("handheld"), TEXT("hand-held"), TEXT("hand held"), TEXT("shaky"), TEXT("shaking"), TEXT("unsteady"), TEXT("documentary style") }))
	{
		if (ContainsAny(Text, { TEXT("slightly"), TEXT("subtle"), TEXT("subtly"), TEXT("a little"), TEXT("gentle") }))
		{
			OutSegment.HandheldIntensity = 0.4f;
		}
		else if (ContainsAny(Text, { TEXT("very"), TEXT("heavy"), TEXT("heavily"), TEXT("violent"), TEXT("violently"), TEXT("extreme") }))
		{
			OutSegment.HandheldIntensity = 1.5f;
		}
		else
		{
			OutSegment.HandheldIntensity = 0.8f;
		}
		bRecognizedAnything = true;
	}

	if (ContainsAny(Text, { TEXT("dutch"), TEXT("canted"), TEXT("dutch angle"), TEXT("canted angle") }))
	{
		OutSegment.DutchAngleDeg = 12.0f;
		bRecognizedAnything = true;
	}

	// ---- Post-process effects ----------------------------------------------------
	// One heaviness reading per clause; it scales every effect mentioned in it.
	const bool bSubtleFx = ContainsAny(Text, { TEXT("slight"), TEXT("slightly"), TEXT("subtle"), TEXT("subtly"), TEXT("a little"), TEXT("light"), TEXT("gentle") });
	const bool bHeavyFx = ContainsAny(Text, { TEXT("heavy"), TEXT("heavily"), TEXT("strong"), TEXT("intense"), TEXT("extreme"), TEXT("very") });
	auto FxIntensity = [bSubtleFx, bHeavyFx](float Subtle, float Normal, float Heavy)
	{
		return bHeavyFx ? Heavy : (bSubtleFx ? Subtle : Normal);
	};

	if (ContainsAny(Text, { TEXT("film grain"), TEXT("grainy"), TEXT("grain") }))
	{
		OutSegment.FilmGrainIntensity = FxIntensity(0.15f, 0.4f, 0.8f);
		bRecognizedAnything = true;
	}
	if (ContainsAny(Text, { TEXT("vignette"), TEXT("vignetting") }))
	{
		OutSegment.VignetteIntensity = FxIntensity(0.3f, 0.6f, 0.9f);
		bRecognizedAnything = true;
	}
	if (ContainsAny(Text, { TEXT("chromatic aberration"), TEXT("chromatic"), TEXT("color fringing"), TEXT("fringing") }))
	{
		OutSegment.ChromaticAberrationIntensity = FxIntensity(1.0f, 2.0f, 4.0f);
		bRecognizedAnything = true;
	}
	if (ContainsAny(Text, { TEXT("bloom"), TEXT("glow"), TEXT("glowing") }))
	{
		OutSegment.BloomIntensity = FxIntensity(1.0f, 1.5f, 3.0f);
		bRecognizedAnything = true;
	}
	if (ContainsAny(Text, { TEXT("lens flare"), TEXT("lens flares"), TEXT("flare"), TEXT("flares") }))
	{
		OutSegment.LensFlareIntensity = FxIntensity(0.5f, 2.0f, 4.0f);
		bRecognizedAnything = true;
	}

	// ---- Lighting / time of day ---------------------------------------------
	// Most specific phrases first ("midnight" before "night", "golden hour"
	// before anything mentioning "hour").
	if (ContainsAny(Text, { TEXT("midnight"), TEXT("dead of night") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Midnight;
	}
	else if (ContainsAny(Text, { TEXT("night"), TEXT("nighttime"), TEXT("night-time"), TEXT("at night"), TEXT("moonlight"), TEXT("moonlit"), TEXT("night falls") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Night;
	}
	else if (ContainsAny(Text, { TEXT("dawn"), TEXT("sunrise"), TEXT("daybreak"), TEXT("first light") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Dawn;
	}
	else if (ContainsAny(Text, { TEXT("golden hour"), TEXT("magic hour"), TEXT("golden light") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::GoldenHour;
	}
	else if (ContainsAny(Text, { TEXT("sunset"), TEXT("setting sun"), TEXT("sun sets"), TEXT("sundown") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Sunset;
	}
	else if (ContainsAny(Text, { TEXT("dusk"), TEXT("twilight"), TEXT("evening") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Dusk;
	}
	else if (ContainsAny(Text, { TEXT("noon"), TEXT("midday"), TEXT("broad daylight"), TEXT("high sun") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Noon;
	}
	else if (ContainsPhrase(Text, TEXT("afternoon")))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Afternoon;
	}
	else if (ContainsPhrase(Text, TEXT("morning")))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Morning;
	}
	else if (ContainsAny(Text, { TEXT("overcast"), TEXT("cloudy"), TEXT("gloomy"), TEXT("grey sky"), TEXT("gray sky"), TEXT("grey skies"), TEXT("gray skies") }))
	{
		OutSegment.TimeOfDay = ECineTimeOfDay::Overcast;
	}
	if (OutSegment.TimeOfDay != ECineTimeOfDay::Unchanged)
	{
		bRecognizedAnything = true;
	}

	// ---- Fog / atmosphere -----------------------------------------------------
	if (ContainsAny(Text, { TEXT("no fog"), TEXT("fog lifts"), TEXT("fog clears"), TEXT("clear air") }))
	{
		OutSegment.FogDensity = 0.002f;
		bRecognizedAnything = true;
	}
	else if (ContainsAny(Text, { TEXT("fog"), TEXT("foggy"), TEXT("mist"), TEXT("misty"), TEXT("haze"), TEXT("hazy") }))
	{
		const bool bThick = ContainsAny(Text, { TEXT("thick"), TEXT("dense"), TEXT("heavy"), TEXT("soupy") });
		const bool bThin = ContainsAny(Text, { TEXT("light"), TEXT("slight"), TEXT("thin"), TEXT("wispy"), TEXT("faint"), TEXT("gentle"), TEXT("a bit of") });
		// Mist and haze without the word "fog" read as the lighter end by default.
		const bool bMistOnly = !ContainsAny(Text, { TEXT("fog"), TEXT("foggy") });
		OutSegment.FogDensity = bThick ? 0.15f : ((bThin || bMistOnly) ? 0.03f : 0.06f);
		bRecognizedAnything = true;
	}

	if (ContainsAny(Text, { TEXT("god rays"), TEXT("god-rays"), TEXT("godrays"), TEXT("light shafts"), TEXT("light shaft"), TEXT("sunbeams"), TEXT("sun beams"), TEXT("crepuscular") }))
	{
		OutSegment.bGodRays = true;
		bRecognizedAnything = true;
	}
	if (ContainsPhrase(Text, TEXT("volumetric")))
	{
		OutSegment.bVolumetricFog = true;
		bRecognizedAnything = true;
	}

	// ---- Timing -----------------------------------------------------------------
	bool bExplicitDuration = false;
	if (MatchNumber(Text, TEXT("(\\d+(?:\\.\\d+)?)\\s*(?:s\\b|sec\\b|secs\\b|second|seconds)"), Number) && Number > 0.0)
	{
		OutSegment.DurationSeconds = Number;
		bExplicitDuration = true;
		bRecognizedAnything = true;
	}
	else
	{
		switch (OutSegment.Move)
		{
		case ECineMoveType::Static:    OutSegment.DurationSeconds = 3.0; break;
		case ECineMoveType::OrbitCW:
		case ECineMoveType::OrbitCCW:  OutSegment.DurationSeconds = 6.0; break;
		case ECineMoveType::Flyover:   OutSegment.DurationSeconds = 8.0; break;
		case ECineMoveType::PanLeft:
		case ECineMoveType::PanRight:
		case ECineMoveType::TiltUp:
		case ECineMoveType::TiltDown:
		case ECineMoveType::ZoomIn:
		case ECineMoveType::ZoomOut:   OutSegment.DurationSeconds = 3.0; break;
		default:                       OutSegment.DurationSeconds = 4.0; break;
		}
	}

	if (!bExplicitDuration)
	{
		if (ContainsAny(Text, { TEXT("slow"), TEXT("slowly"), TEXT("gradual"), TEXT("gradually"), TEXT("creep"), TEXT("creeping") }))
		{
			OutSegment.DurationSeconds *= 1.6;
		}
		else if (ContainsAny(Text, { TEXT("fast"), TEXT("quick"), TEXT("quickly"), TEXT("rapid"), TEXT("rapidly"), TEXT("snap"), TEXT("whip") }))
		{
			OutSegment.DurationSeconds *= 0.6;
		}
	}

	if (ContainsAny(Text, { TEXT("constant speed"), TEXT("linear"), TEXT("no easing") }))
	{
		OutSegment.Easing = ECineEasing::Linear;
	}

	if (!OutSegment.TargetActor.IsValid() && OutSegment.TargetLabel.IsEmpty())
	{
		OutSegment.bLookAtTarget = false;
		if (bIsOrbit)
		{
			OutSegment.ParseNotes.Add(TEXT("Orbit without a recognizable target — orbiting the point in front of the viewport camera."));
		}
	}

	return bRecognizedAnything || !Text.IsEmpty();
}

bool FShotGrammarParser::BuildShotPlan(const FString& Description, const FCineSceneContext& Scene, FCineShotPlan& OutPlan, FText& OutError)
{
	OutPlan = FCineShotPlan();

	if (Description.TrimStartAndEnd().IsEmpty())
	{
		OutError = LOCTEXT("EmptyDescription", "Type a shot description first — e.g. \"slow close-up orbit around the Knight, shallow focus\".");
		return false;
	}

	const TArray<FString> Clauses = SplitIntoSegments(Description);
	if (Clauses.Num() == 0)
	{
		OutError = LOCTEXT("NoClauses", "Couldn't find anything to interpret in that description.");
		return false;
	}

	for (const FString& Clause : Clauses)
	{
		FCineShotSegment Segment;
		if (ParseSegment(Clause, Scene, Segment))
		{
			OutPlan.Segments.Add(MoveTemp(Segment));
		}
	}

	if (OutPlan.Segments.Num() == 0)
	{
		OutError = LOCTEXT("NothingRecognized", "No shots recognized. Try move words like orbit, push in, pull back, crane up, pan, flyover — see the vocabulary help below.");
		return false;
	}

	// "one take" / "continuous" chains the whole description onto a single camera.
	const FString LowerDescription = Description.ToLower();
	if (CineDirectorGrammar::ContainsAny(LowerDescription,
		{ TEXT("one take"), TEXT("single take"), TEXT("one continuous"), TEXT("continuous"), TEXT("long take"), TEXT("uncut"), TEXT("oner") }))
	{
		OutPlan.bOneContinuousShot = true;
	}

	return true;
}

FText FShotGrammarParser::GetVocabularyHelpText()
{
	return LOCTEXT("VocabHelp",
		"Write one or more shot clauses, separated by periods or the word \"then\".\n"
		"Each clause normally becomes its own camera and cut; say \"one take\" or\n"
		"\"continuous\" (or tick the checkbox) to chain every move onto a single camera.\n"
		"Each clause can mention:\n\n"
		"  Moves:    orbit / circle around, push in / dolly in, pull back, truck or track left/right,\n"
		"            crane / boom up or down, pan left/right, tilt up/down, zoom in/out,\n"
		"            flyover / drone shot, static / locked\n"
		"  Target:   any actor label from the outliner (\"around the Knight\", \"on the tower\")\n"
		"  Framing:  extreme close-up, close-up, medium, wide, establishing\n"
		"  Angle:    low angle, high angle, overhead / bird's eye, from behind, from the left,\n"
		"            over the shoulder\n"
		"  Lens:     \"50mm\", wide-angle, portrait, telephoto; aperture as \"f/1.8\",\n"
		"            shallow focus, deep focus\n"
		"  Focus:    focus on <actor>, rack focus from <actor> to <actor>\n"
		"  Effects:  handheld / shaky (slightly, very), dutch angle,\n"
		"            film grain, vignette, chromatic aberration, bloom, lens flares\n"
		"  Lighting: at dawn / morning / noon / afternoon / golden hour / sunset / dusk /\n"
		"            night / midnight / overcast — keys the level's sun per shot;\n"
		"            fog (light, heavy, \"no fog\"), god rays, volumetric fog\n"
		"  Timing:   \"over 8 seconds\", \"for 3s\", slow, fast\n"
		"  Amount:   \"180 degrees\", half / full orbit, \"by 4 meters\"\n\n"
		"Example:\n"
		"  Establishing drone shot over the Castle for 6 seconds. Then a slow 180 degree\n"
		"  orbit around the Knight, close-up, 85mm, shallow focus. Then push in on the\n"
		"  Door, handheld, then rack focus from the Knight to the Door.");
}

#undef LOCTEXT_NAMESPACE
