// Copyright Roundtree. All Rights Reserved.

#include "CineBodyGrammar.h"

#include "Internationalization/Regex.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorBodyGrammar, Log, All);

namespace
{
	bool ContainsAny(const FString& Text, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Text.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}
}

FString FCineBodySpec::Describe() const
{
	FString Out = bSitting ? TEXT("sitting") : TEXT("standing");
	if (bSmoke)
	{
		Out += TEXT(", smoking");
	}
	if (bLookAround)
	{
		Out += TEXT(", looking around");
	}
	switch (Mood)
	{
	case ECineBodyMood::Nervous: Out += TEXT(", nervous"); break;
	case ECineBodyMood::Chill: Out += TEXT(", chill"); break;
	case ECineBodyMood::Alert: Out += TEXT(", alert"); break;
	default: break;
	}
	if (!FMath::IsNearlyEqual(MoodIntensity, 1.0f))
	{
		Out += FString::Printf(TEXT(" (x%.2f)"), MoodIntensity);
	}
	Out += FString::Printf(TEXT(", %.0fs"), DurationSeconds);
	return Out;
}

FCineBodyMoodDials FCineBodyMoodDials::For(ECineBodyMood Mood, float Intensity)
{
	FCineBodyMoodDials Dials;
	const float I = FMath::Clamp(Intensity, 0.4f, 1.6f);
	switch (Mood)
	{
	case ECineBodyMood::Nervous:
		Dials.Tempo = FMath::Lerp(1.0f, 1.25f, I);
		Dials.Ambient = FMath::Lerp(1.0f, 1.4f, I);
		Dials.LagMul = FMath::Lerp(1.0f, 0.75f, I);      // twitchier
		Dials.OvershootMul = FMath::Lerp(1.0f, 1.35f, I);
		Dials.LookYawMul = FMath::Lerp(1.0f, 1.15f, I);
		Dials.bFootTap = true;
		Dials.bKneeBounce = true;
		break;
	case ECineBodyMood::Chill:
		Dials.Tempo = FMath::Lerp(1.0f, 0.78f, I);
		Dials.Ambient = FMath::Lerp(1.0f, 0.8f, I);
		Dials.OvershootMul = FMath::Lerp(1.0f, 0.8f, I);
		Dials.SlouchDeg = FMath::Lerp(0.0f, 5.0f, I);
		Dials.LookYawMul = FMath::Lerp(1.0f, 0.8f, I);
		break;
	case ECineBodyMood::Alert:
		Dials.Tempo = FMath::Lerp(1.0f, 1.1f, I);
		Dials.Ambient = FMath::Lerp(1.0f, 0.7f, I);
		Dials.SlouchDeg = FMath::Lerp(0.0f, -7.0f, I);   // upright
		Dials.LookYawMul = FMath::Lerp(1.0f, 1.3f, I);
		break;
	default:
		break;
	}
	return Dials;
}

FCineBodySpec FCineBodyGrammar::Parse(const FString& Description, int32 Seed)
{
	FCineBodySpec Spec;
	Spec.Seed = Seed;
	const FString Text = Description.ToLower();

	Spec.bSitting = ContainsAny(Text, { TEXT("sit"), TEXT("seated"), TEXT("bench"), TEXT("stool"), TEXT("chair"), TEXT("couch") });
	Spec.bSmoke = ContainsAny(Text, { TEXT("smok"), TEXT("cig"), TEXT("joint"), TEXT("blunt") });
	Spec.bLookAround = ContainsAny(Text, { TEXT("look around"), TEXT("looking around"), TEXT("look-around"),
		TEXT("scan"), TEXT("glanc"), TEXT("watchful"), TEXT("survey"), TEXT("keeping watch"), TEXT("lookout") });

	if (ContainsAny(Text, { TEXT("nervous"), TEXT("anxious"), TEXT("jitter"), TEXT("paranoid"), TEXT("twitchy"), TEXT("on edge"), TEXT("sketchy") }))
	{
		Spec.Mood = ECineBodyMood::Nervous;
	}
	else if (ContainsAny(Text, { TEXT("chill"), TEXT("relaxed"), TEXT("calm"), TEXT("lazy"), TEXT("tired"), TEXT("mellow"), TEXT("laid back"), TEXT("laid-back") }))
	{
		Spec.Mood = ECineBodyMood::Chill;
	}
	else if (ContainsAny(Text, { TEXT("alert"), TEXT("tense"), TEXT("vigilant"), TEXT("on guard"), TEXT("wary") }))
	{
		Spec.Mood = ECineBodyMood::Alert;
	}

	if (ContainsAny(Text, { TEXT("very"), TEXT("extremely"), TEXT("super"), TEXT("really") }))
	{
		Spec.MoodIntensity = 1.35f;
	}
	else if (ContainsAny(Text, { TEXT("slightly"), TEXT("a bit"), TEXT("a little"), TEXT("kind of"), TEXT("kinda") }))
	{
		Spec.MoodIntensity = 0.65f;
	}

	// "for 20 seconds" / "20s" / "20 sec"
	{
		const FRegexPattern Pattern(TEXT("(\\d+)\\s*(?:seconds|second|secs|sec|s\\b)"));
		FRegexMatcher Matcher(Pattern, Text);
		if (Matcher.FindNext())
		{
			Spec.DurationSeconds = FMath::Clamp(FCString::Atof(*Matcher.GetCaptureGroup(1)), 4.0f, 120.0f);
		}
	}
	if (Spec.DurationSeconds <= 0.0f)
	{
		if (Spec.bSmoke && Spec.bLookAround) { Spec.DurationSeconds = 20.0f; }
		else if (Spec.bSmoke) { Spec.DurationSeconds = 16.0f; }
		else if (Spec.bLookAround) { Spec.DurationSeconds = 13.0f; }
		else { Spec.DurationSeconds = 12.0f; }
	}

	UE_LOG(LogCineDirectorBodyGrammar, Display, TEXT("Body prompt \"%s\" → %s"), *Description, *Spec.Describe());
	return Spec;
}

FText FCineBodyGrammar::GetVocabularyHelpText()
{
	return FText::FromString(TEXT(
		"Base: sitting (sit/seated/bench/stool/chair/couch) or standing (default).\n"
		"Activities: smoking (smoke/cig/joint), looking around (look around/scanning/glancing/keeping watch). "
		"Combine freely; none = breathing idle.\n"
		"Mood: nervous (anxious/jittery/paranoid), chill (relaxed/lazy/tired), alert (tense/vigilant/wary) — "
		"with very/slightly modifiers. Mood drives tempo, slouch, tick habits (foot taps, knee bounce) and "
		"how snappily joints move.\n"
		"Duration: \"for 20 seconds\" (default picked from the activities). Loops cleanly.\n"
		"Example: \"sitting on the bench smoking, very nervous, looking around, for 25 seconds\""));
}
