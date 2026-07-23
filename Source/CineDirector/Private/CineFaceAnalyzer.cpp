// Copyright Roundtree. All Rights Reserved.

#include "CineFaceAnalyzer.h"

#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorFace, Log, All);

const TCHAR* CineFaceSlotName(ECineFaceSlot Slot)
{
	switch (Slot)
	{
	case ECineFaceSlot::JawOpen:        return TEXT("JawOpen");
	case ECineFaceSlot::MouthClose:     return TEXT("MouthClose");
	case ECineFaceSlot::MouthWide:      return TEXT("MouthWide");
	case ECineFaceSlot::MouthPucker:    return TEXT("MouthPucker");
	case ECineFaceSlot::MouthFunnel:    return TEXT("MouthFunnel");
	case ECineFaceSlot::MouthSmile:     return TEXT("MouthSmile");
	case ECineFaceSlot::MouthFrown:     return TEXT("MouthFrown");
	case ECineFaceSlot::MouthPress:     return TEXT("MouthPress");
	case ECineFaceSlot::MouthUpperUp:   return TEXT("MouthUpperUp");
	case ECineFaceSlot::MouthLowerDown: return TEXT("MouthLowerDown");
	case ECineFaceSlot::NoseSneer:      return TEXT("NoseSneer");
	case ECineFaceSlot::BrowUp:         return TEXT("BrowUp");
	case ECineFaceSlot::BrowDown:       return TEXT("BrowDown");
	case ECineFaceSlot::BrowSad:        return TEXT("BrowSad");
	case ECineFaceSlot::EyeBlink:       return TEXT("EyeBlink");
	case ECineFaceSlot::EyeWide:        return TEXT("EyeWide");
	case ECineFaceSlot::EyeSquint:      return TEXT("EyeSquint");
	case ECineFaceSlot::EyeLookLeft:    return TEXT("EyeLookLeft");
	case ECineFaceSlot::EyeLookRight:   return TEXT("EyeLookRight");
	case ECineFaceSlot::EyeLookUp:      return TEXT("EyeLookUp");
	case ECineFaceSlot::EyeLookDown:    return TEXT("EyeLookDown");
	case ECineFaceSlot::ExprHappy:      return TEXT("ExprHappy");
	case ECineFaceSlot::ExprAngry:      return TEXT("ExprAngry");
	case ECineFaceSlot::ExprSad:        return TEXT("ExprSad");
	case ECineFaceSlot::ExprSurprised:  return TEXT("ExprSurprised");
	default:                            return TEXT("Unknown");
	}
}

namespace
{
	/** Lowercase with every non-alphanumeric stripped, so Mouth_Smile_L == mouthsmilel. */
	FString NormalizeName(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (TCHAR C : In)
		{
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	struct FNamePattern
	{
		const TCHAR* Contains;
		ECineFaceSlot Slot;
		float Scale;
	};

	/**
	 * Exact (normalized) matches first: ARKit's 52, Oculus visemes, VRM/MMD,
	 * and the Reallusion/CC viseme set — the names audio-to-face pipelines emit.
	 */
	const TMap<FString, TPair<ECineFaceSlot, float>>& ExactTable()
	{
		static TMap<FString, TPair<ECineFaceSlot, float>> Table;
		if (Table.Num() == 0)
		{
			auto Add = [](const TCHAR* Name, ECineFaceSlot Slot, float Scale = 1.0f)
			{
				Table.Add(NormalizeName(Name), TPair<ECineFaceSlot, float>(Slot, Scale));
			};
			// ARKit
			Add(TEXT("jawOpen"), ECineFaceSlot::JawOpen);
			Add(TEXT("mouthClose"), ECineFaceSlot::MouthClose);
			Add(TEXT("mouthPucker"), ECineFaceSlot::MouthPucker);
			Add(TEXT("mouthFunnel"), ECineFaceSlot::MouthFunnel);
			Add(TEXT("mouthSmileLeft"), ECineFaceSlot::MouthSmile);
			Add(TEXT("mouthSmileRight"), ECineFaceSlot::MouthSmile);
			Add(TEXT("mouthFrownLeft"), ECineFaceSlot::MouthFrown);
			Add(TEXT("mouthFrownRight"), ECineFaceSlot::MouthFrown);
			Add(TEXT("mouthPressLeft"), ECineFaceSlot::MouthPress);
			Add(TEXT("mouthPressRight"), ECineFaceSlot::MouthPress);
			Add(TEXT("mouthStretchLeft"), ECineFaceSlot::MouthWide);
			Add(TEXT("mouthStretchRight"), ECineFaceSlot::MouthWide);
			Add(TEXT("mouthUpperUpLeft"), ECineFaceSlot::MouthUpperUp);
			Add(TEXT("mouthUpperUpRight"), ECineFaceSlot::MouthUpperUp);
			Add(TEXT("mouthLowerDownLeft"), ECineFaceSlot::MouthLowerDown);
			Add(TEXT("mouthLowerDownRight"), ECineFaceSlot::MouthLowerDown);
			Add(TEXT("noseSneerLeft"), ECineFaceSlot::NoseSneer);
			Add(TEXT("noseSneerRight"), ECineFaceSlot::NoseSneer);
			Add(TEXT("browInnerUp"), ECineFaceSlot::BrowSad);
			Add(TEXT("browOuterUpLeft"), ECineFaceSlot::BrowUp);
			Add(TEXT("browOuterUpRight"), ECineFaceSlot::BrowUp);
			Add(TEXT("browDownLeft"), ECineFaceSlot::BrowDown);
			Add(TEXT("browDownRight"), ECineFaceSlot::BrowDown);
			Add(TEXT("eyeBlinkLeft"), ECineFaceSlot::EyeBlink);
			Add(TEXT("eyeBlinkRight"), ECineFaceSlot::EyeBlink);
			Add(TEXT("eyeWideLeft"), ECineFaceSlot::EyeWide);
			Add(TEXT("eyeWideRight"), ECineFaceSlot::EyeWide);
			Add(TEXT("eyeSquintLeft"), ECineFaceSlot::EyeSquint);
			Add(TEXT("eyeSquintRight"), ECineFaceSlot::EyeSquint);
			// ARKit gaze
			Add(TEXT("eyeLookInLeft"), ECineFaceSlot::EyeLookRight);   // left eye in = look right
			Add(TEXT("eyeLookInRight"), ECineFaceSlot::EyeLookLeft);
			Add(TEXT("eyeLookOutLeft"), ECineFaceSlot::EyeLookLeft);
			Add(TEXT("eyeLookOutRight"), ECineFaceSlot::EyeLookRight);
			Add(TEXT("eyeLookUpLeft"), ECineFaceSlot::EyeLookUp);
			Add(TEXT("eyeLookUpRight"), ECineFaceSlot::EyeLookUp);
			Add(TEXT("eyeLookDownLeft"), ECineFaceSlot::EyeLookDown);
			Add(TEXT("eyeLookDownRight"), ECineFaceSlot::EyeLookDown);
			// Oculus visemes
			Add(TEXT("viseme_aa"), ECineFaceSlot::JawOpen);
			Add(TEXT("viseme_E"), ECineFaceSlot::MouthWide);
			Add(TEXT("viseme_ih"), ECineFaceSlot::MouthWide, 0.7f);
			Add(TEXT("viseme_oh"), ECineFaceSlot::MouthFunnel);
			Add(TEXT("viseme_ou"), ECineFaceSlot::MouthPucker);
			Add(TEXT("viseme_PP"), ECineFaceSlot::MouthClose);
			Add(TEXT("viseme_SS"), ECineFaceSlot::MouthWide, 0.5f);
			Add(TEXT("viseme_FF"), ECineFaceSlot::MouthPress, 0.6f);
			// VRM / MMD-style: A-I-U-E-O vowel visemes (exclusive), full-face emotions, gaze
			Add(TEXT("A"), ECineFaceSlot::JawOpen);
			Add(TEXT("I"), ECineFaceSlot::MouthWide);
			Add(TEXT("U"), ECineFaceSlot::MouthPucker);
			Add(TEXT("E"), ECineFaceSlot::MouthWide, 0.6f);
			Add(TEXT("O"), ECineFaceSlot::MouthFunnel);
			// Full-face expression morphs — driven whole by the baker's Expr* slots
			Add(TEXT("Joy"), ECineFaceSlot::ExprHappy);
			Add(TEXT("Fun"), ECineFaceSlot::ExprHappy, 0.85f);
			Add(TEXT("Happy"), ECineFaceSlot::ExprHappy);
			Add(TEXT("Angry"), ECineFaceSlot::ExprAngry);
			Add(TEXT("Anger"), ECineFaceSlot::ExprAngry);
			Add(TEXT("Sorrow"), ECineFaceSlot::ExprSad);
			Add(TEXT("Sad"), ECineFaceSlot::ExprSad);
			Add(TEXT("Surprised"), ECineFaceSlot::ExprSurprised);
			Add(TEXT("Surprise"), ECineFaceSlot::ExprSurprised);
			// Also bind micro-slots so partial emotion components still land when present
			Add(TEXT("Smile"), ECineFaceSlot::MouthSmile);
			// VRM blink variants
			Add(TEXT("Blink"), ECineFaceSlot::EyeBlink);
			Add(TEXT("Blink_L"), ECineFaceSlot::EyeBlink);
			Add(TEXT("Blink_R"), ECineFaceSlot::EyeBlink);
			Add(TEXT("BlinkLeft"), ECineFaceSlot::EyeBlink);
			Add(TEXT("BlinkRight"), ECineFaceSlot::EyeBlink);
			Add(TEXT("Eye_Close"), ECineFaceSlot::EyeBlink);
			Add(TEXT("EyeClose"), ECineFaceSlot::EyeBlink);
			// VRM gaze
			Add(TEXT("LookLeft"), ECineFaceSlot::EyeLookLeft);
			Add(TEXT("LookRight"), ECineFaceSlot::EyeLookRight);
			Add(TEXT("LookUp"), ECineFaceSlot::EyeLookUp);
			Add(TEXT("LookDown"), ECineFaceSlot::EyeLookDown);
			Add(TEXT("Look_Left"), ECineFaceSlot::EyeLookLeft);
			Add(TEXT("Look_Right"), ECineFaceSlot::EyeLookRight);
			Add(TEXT("Look_Up"), ECineFaceSlot::EyeLookUp);
			Add(TEXT("Look_Down"), ECineFaceSlot::EyeLookDown);
			// Reallusion / Character Creator
			Add(TEXT("V_Open"), ECineFaceSlot::JawOpen);
			Add(TEXT("V_Wide"), ECineFaceSlot::MouthWide);
			Add(TEXT("V_Tight_O"), ECineFaceSlot::MouthPucker);
			Add(TEXT("V_Explosive"), ECineFaceSlot::MouthClose);
			Add(TEXT("V_Lip_Open"), ECineFaceSlot::MouthFunnel, 0.7f);
			Add(TEXT("V_Dental_Lip"), ECineFaceSlot::MouthPress, 0.6f);
		}
		return Table;
	}

	/** Fuzzy fallbacks, tested in order — first hit wins for a given morph. */
	const FNamePattern* FuzzyTable(int32& OutNum)
	{
		static const FNamePattern Patterns[] = {
			// Gaze before generic "eye" patterns so LookLeft doesn't become blink.
			// Void GLB/FBX exports use accessory-prefixed names (NavyLook*, TwirlLook*, …).
			{ TEXT("lookleft"), ECineFaceSlot::EyeLookLeft, 1.0f },
			{ TEXT("lookright"), ECineFaceSlot::EyeLookRight, 1.0f },
			{ TEXT("lookup"), ECineFaceSlot::EyeLookUp, 1.0f },
			{ TEXT("lookdown"), ECineFaceSlot::EyeLookDown, 1.0f },
			{ TEXT("eyelookleft"), ECineFaceSlot::EyeLookLeft, 1.0f },
			{ TEXT("eyelookright"), ECineFaceSlot::EyeLookRight, 1.0f },
			{ TEXT("eyelookup"), ECineFaceSlot::EyeLookUp, 1.0f },
			{ TEXT("eyelookdown"), ECineFaceSlot::EyeLookDown, 1.0f },
			{ TEXT("gazeleft"), ECineFaceSlot::EyeLookLeft, 1.0f },
			{ TEXT("gazeright"), ECineFaceSlot::EyeLookRight, 1.0f },
			// Prefixed void look sets (Navy/Twirl/Stoned/Lime) also match *lookleft via
			// the generic patterns above; keep explicit names for clarity in tables.
			{ TEXT("jawopen"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("mouthopen"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("jawdrop"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("openmouth"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("mouthah"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("blink"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyesclosed"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyeclose"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyewide"), ECineFaceSlot::EyeWide, 1.0f },
			{ TEXT("squint"), ECineFaceSlot::EyeSquint, 1.0f },
			{ TEXT("smile"), ECineFaceSlot::MouthSmile, 1.0f },
			{ TEXT("happy"), ECineFaceSlot::ExprHappy, 0.9f },
			{ TEXT("joy"), ECineFaceSlot::ExprHappy, 1.0f },
			{ TEXT("angry"), ECineFaceSlot::ExprAngry, 1.0f },
			{ TEXT("anger"), ECineFaceSlot::ExprAngry, 1.0f },
			{ TEXT("sorrow"), ECineFaceSlot::ExprSad, 1.0f },
			{ TEXT("frown"), ECineFaceSlot::MouthFrown, 1.0f },
			{ TEXT("sadmouth"), ECineFaceSlot::MouthFrown, 1.0f },
			{ TEXT("surprised"), ECineFaceSlot::ExprSurprised, 1.0f },
			{ TEXT("surprise"), ECineFaceSlot::ExprSurprised, 1.0f },
			{ TEXT("pucker"), ECineFaceSlot::MouthPucker, 1.0f },
			{ TEXT("kiss"), ECineFaceSlot::MouthPucker, 1.0f },
			{ TEXT("funnel"), ECineFaceSlot::MouthFunnel, 1.0f },
			{ TEXT("mouthwide"), ECineFaceSlot::MouthWide, 1.0f },
			{ TEXT("stretch"), ECineFaceSlot::MouthWide, 0.8f },
			{ TEXT("upperlipraise"), ECineFaceSlot::MouthUpperUp, 1.0f },
			{ TEXT("mouthupperup"), ECineFaceSlot::MouthUpperUp, 1.0f },
			{ TEXT("upperlipup"), ECineFaceSlot::MouthUpperUp, 1.0f },
			{ TEXT("lowerlipdepress"), ECineFaceSlot::MouthLowerDown, 1.0f },
			{ TEXT("mouthlowerdown"), ECineFaceSlot::MouthLowerDown, 1.0f },
			{ TEXT("lowerlipdown"), ECineFaceSlot::MouthLowerDown, 1.0f },
			{ TEXT("mouthpress"), ECineFaceSlot::MouthPress, 1.0f },
			{ TEXT("lipspressed"), ECineFaceSlot::MouthPress, 1.0f },
			{ TEXT("mouthclose"), ECineFaceSlot::MouthClose, 1.0f },
			{ TEXT("sneer"), ECineFaceSlot::NoseSneer, 1.0f },
			{ TEXT("snarl"), ECineFaceSlot::NoseSneer, 1.0f },
			{ TEXT("browsup"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browup"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browraise"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browsdown"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browdown"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browfurrow"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("angrybrow"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browinnerup"), ECineFaceSlot::BrowSad, 1.0f },
			{ TEXT("worried"), ECineFaceSlot::BrowSad, 0.8f },
		};
		OutNum = UE_ARRAY_COUNT(Patterns);
		return Patterns;
	}

	/** MetaHuman rig-logic control curves for each slot (left/right where split). */
	void FillMetaHumanProfile(FCineFaceProfile& Profile)
	{
		auto Bind = [&Profile](ECineFaceSlot Slot, std::initializer_list<const TCHAR*> Curves, float Scale = 1.0f)
		{
			for (const TCHAR* Curve : Curves)
			{
				Profile.Slots[(int32)Slot].Add({ FName(Curve), Scale });
			}
		};
		Bind(ECineFaceSlot::JawOpen, { TEXT("CTRL_expressions_jawOpen") });
		Bind(ECineFaceSlot::MouthClose, { TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"), TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR") });
		Bind(ECineFaceSlot::MouthWide, { TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchR") }, 0.8f);
		Bind(ECineFaceSlot::MouthPucker, { TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"), TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR") });
		Bind(ECineFaceSlot::MouthFunnel, { TEXT("CTRL_expressions_mouthLipsFunnelUL"), TEXT("CTRL_expressions_mouthLipsFunnelUR"), TEXT("CTRL_expressions_mouthLipsFunnelDL"), TEXT("CTRL_expressions_mouthLipsFunnelDR") });
		Bind(ECineFaceSlot::MouthSmile, { TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR") });
		Bind(ECineFaceSlot::MouthFrown, { TEXT("CTRL_expressions_mouthCornerDepressL"), TEXT("CTRL_expressions_mouthCornerDepressR") });
		Bind(ECineFaceSlot::MouthPress, { TEXT("CTRL_expressions_mouthPressUL"), TEXT("CTRL_expressions_mouthPressUR"), TEXT("CTRL_expressions_mouthPressDL"), TEXT("CTRL_expressions_mouthPressDR") });
		Bind(ECineFaceSlot::MouthUpperUp, { TEXT("CTRL_expressions_mouthUpperLipRaiseL"), TEXT("CTRL_expressions_mouthUpperLipRaiseR") });
		Bind(ECineFaceSlot::MouthLowerDown, { TEXT("CTRL_expressions_mouthLowerLipDepressL"), TEXT("CTRL_expressions_mouthLowerLipDepressR") });
		Bind(ECineFaceSlot::NoseSneer, { TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_noseWrinkleR") });
		Bind(ECineFaceSlot::BrowUp, { TEXT("CTRL_expressions_browRaiseOuterL"), TEXT("CTRL_expressions_browRaiseOuterR") });
		Bind(ECineFaceSlot::BrowDown, { TEXT("CTRL_expressions_browDownL"), TEXT("CTRL_expressions_browDownR") });
		Bind(ECineFaceSlot::BrowSad, { TEXT("CTRL_expressions_browRaiseInL"), TEXT("CTRL_expressions_browRaiseInR") });
		Bind(ECineFaceSlot::EyeBlink, { TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR") });
		Bind(ECineFaceSlot::EyeWide, { TEXT("CTRL_expressions_eyeWidenL"), TEXT("CTRL_expressions_eyeWidenR") });
		Bind(ECineFaceSlot::EyeSquint, { TEXT("CTRL_expressions_eyeSquintInnerL"), TEXT("CTRL_expressions_eyeSquintInnerR") });
		// MetaHuman gaze — pupil look via eye aim controls
		Bind(ECineFaceSlot::EyeLookLeft, { TEXT("CTRL_eyes_lookLeft") }, 1.0f);
		Bind(ECineFaceSlot::EyeLookRight, { TEXT("CTRL_eyes_lookRight") }, 1.0f);
		Bind(ECineFaceSlot::EyeLookUp, { TEXT("CTRL_eyes_lookUp") }, 1.0f);
		Bind(ECineFaceSlot::EyeLookDown, { TEXT("CTRL_eyes_lookDown") }, 1.0f);
	}
}

FCineFaceProfile FCineFaceAnalyzer::Analyze(USkeletalMesh* Mesh, bool bPreferLayeredArkitMouth)
{
	FCineFaceProfile Profile;
	if (!Mesh)
	{
		Profile.Notes.Add(TEXT("No skeletal mesh."));
		return Profile;
	}
	Profile.MeshName = Mesh->GetName();

	// MetaHuman faces animate through rig-logic control curves, not raw morphs.
	const USkeleton* Skeleton = Mesh->GetSkeleton();
	bool bLooksMetaHuman = Skeleton && Skeleton->GetName().Contains(TEXT("Face_Archetype"));
	if (!bLooksMetaHuman && Skeleton)
	{
		Skeleton->ForEachCurveMetaData([&bLooksMetaHuman](const FName& CurveName, const FCurveMetaData&)
		{
			bLooksMetaHuman |= CurveName.ToString().StartsWith(TEXT("CTRL_expressions_"));
		});
	}
	if (bLooksMetaHuman)
	{
		Profile.bMetaHuman = true;
		FillMetaHumanProfile(Profile);
		Profile.Notes.Add(TEXT("MetaHuman face detected — driving CTRL_expressions rig controls."));
		return Profile;
	}

	const TArray<TObjectPtr<UMorphTarget>>& Morphs = Mesh->GetMorphTargets();
	if (Morphs.Num() == 0)
	{
		Profile.Notes.Add(FString::Printf(TEXT("'%s' has no morph targets — facial animation needs a face mesh with blendshapes."), *Profile.MeshName));
		return Profile;
	}

	int32 NumFuzzy = 0;
	const FNamePattern* Fuzzy = FuzzyTable(NumFuzzy);
	int32 Unrecognized = 0;
	int32 VowelHits = 0; // A/I/U/E/O single-letter style
	// ARKit-ish layered markers (void FBX ships these alongside VRM vowels).
	int32 ArkitHits = 0;
	auto IsArkitMarker = [](const FString& Norm) -> bool
	{
		static const TCHAR* Markers[] = {
			TEXT("jawopen"), TEXT("mouthpucker"), TEXT("mouthfunnel"), TEXT("mouthclose"),
			TEXT("mouthsmileleft"), TEXT("mouthsmileright"), TEXT("mouthstretchleft"), TEXT("mouthstretchright"),
			TEXT("mouthupperupleft"), TEXT("mouthupperupright"), TEXT("mouthlowerdownleft"), TEXT("mouthlowerdownright"),
			TEXT("mouthfrownleft"), TEXT("mouthfrownright"), TEXT("mouthpressleft"), TEXT("mouthpressright"),
			TEXT("browinnerup"), TEXT("browdownleft"), TEXT("browdownright"),
			TEXT("browouterupleft"), TEXT("browouterupright"),
			TEXT("eyeblinkleft"), TEXT("eyeblinkright"), TEXT("eyewideleft"), TEXT("eyewideright"),
		};
		for (const TCHAR* M : Markers)
		{
			if (Norm == M)
			{
				return true;
			}
		}
		return false;
	};
	auto IsVrmVowelName = [](const FString& Norm) -> bool
	{
		return Norm.Len() == 1
			&& (Norm[0] == 'a' || Norm[0] == 'i' || Norm[0] == 'u' || Norm[0] == 'e' || Norm[0] == 'o');
	};
	auto IsVrmFullFaceName = [](const FString& Norm) -> bool
	{
		return Norm == TEXT("joy") || Norm == TEXT("fun") || Norm == TEXT("happy")
			|| Norm == TEXT("angry") || Norm == TEXT("anger")
			|| Norm == TEXT("sorrow") || Norm == TEXT("sad")
			|| Norm == TEXT("surprised") || Norm == TEXT("surprise");
	};

	for (const UMorphTarget* Morph : Morphs)
	{
		if (!Morph)
		{
			continue;
		}
		const FName MorphName = Morph->GetFName();
		const FString Norm = NormalizeName(MorphName.ToString());
		if (IsArkitMarker(Norm))
		{
			++ArkitHits;
		}

		if (const TPair<ECineFaceSlot, float>* Exact = ExactTable().Find(Norm))
		{
			Profile.Slots[(int32)Exact->Key].Add({ MorphName, Exact->Value });
			if (IsVrmVowelName(Norm))
			{
				++VowelHits;
			}
			continue;
		}

		bool bMatched = false;
		for (int32 i = 0; i < NumFuzzy && !bMatched; ++i)
		{
			if (Norm.Contains(Fuzzy[i].Contains))
			{
				Profile.Slots[(int32)Fuzzy[i].Slot].Add({ MorphName, Fuzzy[i].Scale });
				bMatched = true;
			}
		}
		Unrecognized += bMatched ? 0 : 1;
	}

	// Dual-export voids (VRM vowels + ARKit micros, e.g. grills GLB after transfer).
	// Default: exclusive A/I/U/O (safe). Panel "Layered ARKit mouth" opts into
	// MetaHuman-style layering + jaw co-articulation (may stretch more).
	const bool bArkitRich = ArkitHits >= 6;
	const bool bVrmMouth = VowelHits >= 3;
	const bool bLayeredMouth = bPreferLayeredArkitMouth && bArkitRich;
	if (bArkitRich || bVrmMouth)
	{
		Profile.bLayeredBlendshapes = bArkitRich;
		if (bLayeredMouth)
		{
			// Opt-in MetaHuman path: layer ARKit mouth + keep A for grill jaw.
			Profile.bExclusiveVisemes = false;
		}
		else if (bVrmMouth)
		{
			Profile.bExclusiveVisemes = true;
		}

		int32 StrippedArkitMouth = 0;
		int32 Softened = 0;
		auto IsExclusiveMouthSlot = [](int32 Slot) -> bool
		{
			return Slot == (int32)ECineFaceSlot::JawOpen
				|| Slot == (int32)ECineFaceSlot::MouthWide
				|| Slot == (int32)ECineFaceSlot::MouthPucker
				|| Slot == (int32)ECineFaceSlot::MouthFunnel
				|| Slot == (int32)ECineFaceSlot::MouthClose;
		};
		auto IsArkitMouthish = [&](const FString& Norm) -> bool
		{
			// ARKit mouth/jaw micro names — not single-letter VRM vowels / jawOpen.
			if (IsVrmVowelName(Norm) || Norm == TEXT("jawopen"))
			{
				return false;
			}
			return Norm.StartsWith(TEXT("mouth"))
				|| Norm.StartsWith(TEXT("jaw"))
				|| Norm.Contains(TEXT("viseme"));
		};

		for (int32 Slot = 0; Slot < (int32)ECineFaceSlot::Count; ++Slot)
		{
			TArray<FCineFaceCurveTarget>& Targets = Profile.Slots[Slot];
			for (int32 i = Targets.Num() - 1; i >= 0; --i)
			{
				const FString Norm = NormalizeName(Targets[i].CurveName.ToString());
				// Prefer grill-baked A over ARKit jawOpen on the same slot (double open).
				if (Slot == (int32)ECineFaceSlot::JawOpen && Norm == TEXT("jawopen") && bVrmMouth)
				{
					bool bHasA = false;
					for (const FCineFaceCurveTarget& Other : Targets)
					{
						if (NormalizeName(Other.CurveName.ToString()) == TEXT("a"))
						{
							bHasA = true;
							break;
						}
					}
					if (bHasA)
					{
						Targets.RemoveAt(i);
						++StrippedArkitMouth;
						continue;
					}
				}
				// Default dual-void path: strip ARKit mouth from viseme slots.
				// Layered mode keeps them so co-articulation + ARKit shapes can run.
				if (!bLayeredMouth && bVrmMouth && IsExclusiveMouthSlot(Slot) && IsArkitMouthish(Norm))
				{
					Targets.RemoveAt(i);
					++StrippedArkitMouth;
					continue;
				}
			}
		}

		// Soften stretchy ARKit micros. Layered mouth mode also eases ARKit mouth*
		// slightly so A + mouthPucker don't rubber-band as hard.
		for (int32 Slot = 0; Slot < (int32)ECineFaceSlot::Count; ++Slot)
		{
			for (FCineFaceCurveTarget& T : Profile.Slots[Slot])
			{
				const FString Norm = NormalizeName(T.CurveName.ToString());
				if (Norm.Contains(TEXT("mouthlowerdown")) || Norm.Contains(TEXT("lowerlip")))
				{
					T.Scale *= bLayeredMouth ? 0.40f : 0.45f;
					++Softened;
				}
				else if (bLayeredMouth && IsArkitMouthish(Norm) && IsExclusiveMouthSlot(Slot))
				{
					T.Scale *= 0.72f;
					++Softened;
				}
				else if (Norm.Contains(TEXT("browouterup")) || (Norm.Contains(TEXT("browouter")) && Norm.Contains(TEXT("up")))
					|| Norm == TEXT("browinnerup") || Norm.Contains(TEXT("browinner")))
				{
					T.Scale *= bVrmMouth ? 0.70f : 0.40f;
					++Softened;
				}
			}
		}

		if (bLayeredMouth)
		{
			Profile.Notes.Add(FString::Printf(
				TEXT("Layered ARKit mouth ON: exclusive off, jaw co-articulation on, ARKit mouth micros kept (soft-scaled). %d stretchy targets eased. May stretch more than exclusive mode."),
				Softened));
		}
		else if (bArkitRich && bVrmMouth)
		{
			Profile.Notes.Add(FString::Printf(
				TEXT("Void dual face (exclusive A/I/U/O): stripped %d ARKit mouth curves from viseme slots; soft-scaled %d lower-lip/brow-up targets. Enable \"Layered ARKit mouth\" for MetaHuman-style co-articulation."),
				StrippedArkitMouth, Softened));
		}
		else if (bArkitRich)
		{
			Profile.Notes.Add(FString::Printf(
				TEXT("Layered ARKit face (%d markers). Soft-scaled %d stretchy brow-up/lip targets."),
				ArkitHits, Softened));
		}
		else
		{
			Profile.Notes.Add(TEXT("VRM/MMD-style exclusive vowel morphs — lipsync picks one shape per frame."));
		}
	}

	// EyeBlink dedupe (all faces): voyagers ship eyeBlinkLeft/Right + blink_l/r + blink.
	// Driving all five at auto-blink 1.0 stretches lids into the brow/glasses.
	{
		TArray<FCineFaceCurveTarget>& Blinks = Profile.Slots[(int32)ECineFaceSlot::EyeBlink];
		if (Blinks.Num() > 2)
		{
			auto IsArkitBlink = [](const FString& Norm) -> bool
			{
				return Norm == TEXT("eyeblinkleft") || Norm == TEXT("eyeblinkright")
					|| Norm == TEXT("eyeblinkl") || Norm == TEXT("eyeblinkr");
			};
			auto IsVrmPairBlink = [](const FString& Norm) -> bool
			{
				return Norm == TEXT("blinkl") || Norm == TEXT("blinkr")
					|| Norm == TEXT("blinkleft") || Norm == TEXT("blinkright");
			};
			auto IsMonoBlink = [](const FString& Norm) -> bool
			{
				return Norm == TEXT("blink") || Norm == TEXT("eyeclose") || Norm == TEXT("eye_close")
					|| Norm == TEXT("eyesclosed");
			};

			bool bHasArkit = false;
			bool bHasVrmPair = false;
			for (const FCineFaceCurveTarget& T : Blinks)
			{
				const FString N = NormalizeName(T.CurveName.ToString());
				bHasArkit |= IsArkitBlink(N);
				bHasVrmPair |= IsVrmPairBlink(N);
			}

			// Prefer a single L/R pair: ARKit eyeBlink* if present, else blink_l/r.
			const bool bPreferArkit = bHasArkit;
			int32 Kept = 0;
			int32 Dropped = 0;
			for (int32 i = Blinks.Num() - 1; i >= 0; --i)
			{
				const FString N = NormalizeName(Blinks[i].CurveName.ToString());
				bool bKeep = false;
				if (bPreferArkit)
				{
					bKeep = IsArkitBlink(N);
				}
				else if (bHasVrmPair)
				{
					bKeep = IsVrmPairBlink(N);
				}
				else
				{
					// Only mono left — keep one.
					bKeep = (Kept == 0);
				}

				if (bKeep)
				{
					// Slight ease: even a clean L/R pair at 1.0 is hot on voyager lids.
					Blinks[i].Scale *= 0.85f;
					++Kept;
				}
				else
				{
					Blinks.RemoveAt(i);
					++Dropped;
				}
			}
			if (Dropped > 0)
			{
				Profile.Notes.Add(FString::Printf(
					TEXT("EyeBlink: kept %d morph(s), dropped %d duplicate blink curves (was stacking into lid stretch)."),
					Kept, Dropped));
			}
		}
	}

	Profile.Notes.Add(FString::Printf(TEXT("%d morph targets scanned, %d unrecognized."), Morphs.Num(), Unrecognized));
	if (!Profile.HasSlot(ECineFaceSlot::JawOpen))
	{
		Profile.Notes.Add(TEXT("No jaw/mouth-open morph found — lipsync will be very subtle. Check the mesh's morph names."));
	}
	if (!Profile.HasSlot(ECineFaceSlot::EyeLookLeft) && !Profile.HasSlot(ECineFaceSlot::EyeLookRight)
		&& !Profile.HasSlot(ECineFaceSlot::EyeLookUp) && !Profile.HasSlot(ECineFaceSlot::EyeLookDown))
	{
		Profile.Notes.Add(TEXT("No eye-look morphs found — gaze will stay fixed (blinks still work)."));
	}
	return Profile;
}

FString FCineFaceAnalyzer::DescribeProfile(const FCineFaceProfile& Profile)
{
	TArray<FString> Mapped;
	TArray<FString> Missing;
	for (int32 i = 0; i < (int32)ECineFaceSlot::Count; ++i)
	{
		if (Profile.Slots[i].Num() > 0)
		{
			Mapped.Add(FString::Printf(TEXT("%s(%d)"), CineFaceSlotName((ECineFaceSlot)i), Profile.Slots[i].Num()));
		}
		else
		{
			Missing.Add(CineFaceSlotName((ECineFaceSlot)i));
		}
	}

	FString Out = FString::Printf(TEXT("%s: mapped %d/%d face slots"),
		*Profile.MeshName, Profile.NumMappedSlots(), (int32)ECineFaceSlot::Count);
	if (Mapped.Num() > 0)
	{
		Out += FString::Printf(TEXT(" — %s"), *FString::Join(Mapped, TEXT(", ")));
	}
	if (Missing.Num() > 0 && Missing.Num() < (int32)ECineFaceSlot::Count)
	{
		Out += FString::Printf(TEXT(". Unmapped: %s"), *FString::Join(Missing, TEXT(", ")));
	}
	for (const FString& Note : Profile.Notes)
	{
		Out += TEXT("\n") + Note;
	}
	UE_LOG(LogCineDirectorFace, Log, TEXT("%s"), *Out);
	return Out;
}
