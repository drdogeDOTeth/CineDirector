// Copyright Roundtree. All Rights Reserved.

#include "ShotPlanExecutor.h"

#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkyLight.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTimeUnit.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"

#define LOCTEXT_NAMESPACE "CineDirector"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirector, Log, All);

namespace CineDirectorExec
{
	/**
	 * Mid-face world location on a skeletal actor (VRM/MMD/Mixamo/UE void kits).
	 * Prefers eye midpoint → head/neck blend → head bone. Nudges slightly along
	 * actor forward so aim hits the face surface (grills/nose), not skull center.
	 */
	bool FindHeadWorldLocation(const AActor* Actor, FVector& OutHead)
	{
		if (!Actor)
		{
			return false;
		}

		static const FName PreferredBones[] = {
			FName(TEXT("head")),
			FName(TEXT("Head")),
			FName(TEXT("HEAD")),
			FName(TEXT("Head_M")),
			FName(TEXT("head_M")),
			FName(TEXT("J_Bip_C_Head")),
			FName(TEXT("j_bip_c_head")),
			FName(TEXT("mixamorig:Head")),
			FName(TEXT("mixamorig_Head")),
			FName(TEXT("Bip001-Head")),
			FName(TEXT("Bip001 Head")),
			FName(TEXT("Bip01 Head")),
			FName(TEXT("bone_head")),
			FName(TEXT("J_Head")),
			FName(TEXT("Face")),
			FName(TEXT("face")),
		};

		static const FName NeckBones[] = {
			FName(TEXT("neck")),
			FName(TEXT("Neck")),
			FName(TEXT("J_Bip_C_Neck")),
			FName(TEXT("j_bip_c_neck")),
			FName(TEXT("mixamorig:Neck")),
			FName(TEXT("mixamorig_Neck")),
			FName(TEXT("Bip001-Neck")),
			FName(TEXT("Bip001 Neck")),
		};

		static const FName LeftEyeBones[] = {
			FName(TEXT("eye_L")), FName(TEXT("Eye_L")), FName(TEXT("LeftEye")),
			FName(TEXT("leftEye")), FName(TEXT("EyeLeft")), FName(TEXT("mixamorig:LeftEye")),
			FName(TEXT("J_Adj_L_FaceEye")), FName(TEXT("faceeye_L")),
		};
		static const FName RightEyeBones[] = {
			FName(TEXT("eye_R")), FName(TEXT("Eye_R")), FName(TEXT("RightEye")),
			FName(TEXT("rightEye")), FName(TEXT("EyeRight")), FName(TEXT("mixamorig:RightEye")),
			FName(TEXT("J_Adj_R_FaceEye")), FName(TEXT("faceeye_R")),
		};

		TArray<USkeletalMeshComponent*> Meshes;
		Actor->GetComponents<USkeletalMeshComponent>(Meshes);
		for (USkeletalMeshComponent* Skel : Meshes)
		{
			if (!Skel || !Skel->GetSkeletalMeshAsset())
			{
				continue;
			}

			auto TryBone = [Skel](const FName& Bone, FVector& Out) -> bool
			{
				if (Skel->GetBoneIndex(Bone) != INDEX_NONE)
				{
					Out = Skel->GetBoneLocation(Bone);
					return true;
				}
				// Sockets (some VRM exports expose face markers as sockets only).
				if (Skel->DoesSocketExist(Bone))
				{
					Out = Skel->GetSocketLocation(Bone);
					return true;
				}
				return false;
			};

			// 1) Eyes = strongest face lock (void grills / glasses read correctly).
			FVector EyeL = FVector::ZeroVector, EyeR = FVector::ZeroVector;
			bool bL = false, bR = false;
			for (const FName& Bone : LeftEyeBones)
			{
				if (TryBone(Bone, EyeL)) { bL = true; break; }
			}
			for (const FName& Bone : RightEyeBones)
			{
				if (TryBone(Bone, EyeR)) { bR = true; break; }
			}
			if (bL && bR)
			{
				// Mid-eyes, drop slightly toward nose / grills.
				OutHead = (EyeL + EyeR) * 0.5;
				OutHead.Z -= 4.0;
				OutHead += Actor->GetActorForwardVector() * 6.0;
				return true;
			}

			FVector HeadLoc = FVector::ZeroVector;
			bool bFoundHead = false;
			for (const FName& Bone : PreferredBones)
			{
				if (TryBone(Bone, HeadLoc))
				{
					bFoundHead = true;
					break;
				}
			}

			if (!bFoundHead)
			{
				// Fuzzy fallback: first bone whose name contains "head" (not ends/nubs).
				const FReferenceSkeleton& RefSkel = Skel->GetSkeletalMeshAsset()->GetRefSkeleton();
				const int32 NumBones = RefSkel.GetNum();
				int32 BestIdx = INDEX_NONE;
				int32 BestScore = -1;
				for (int32 i = 0; i < NumBones; ++i)
				{
					const FString Name = RefSkel.GetBoneName(i).ToString().ToLower();
					if (!Name.Contains(TEXT("head")))
					{
						continue;
					}
					if (Name.Contains(TEXT("end")) || Name.Contains(TEXT("nub")) || Name.Contains(TEXT("twist"))
						|| Name.Contains(TEXT("top")) || Name.Contains(TEXT("leaf")))
					{
						continue;
					}
					const int32 Score = 100 - Name.Len() + (Name == TEXT("head") ? 50 : 0);
					if (Score > BestScore)
					{
						BestScore = Score;
						BestIdx = i;
					}
				}
				if (BestIdx != INDEX_NONE)
				{
					HeadLoc = Skel->GetBoneLocation(RefSkel.GetBoneName(BestIdx));
					bFoundHead = true;
				}
			}

			if (!bFoundHead)
			{
				continue;
			}

			// VRM / void head bones sit high. Prefer mid-face (eyes–nose), not throat.
			FVector NeckLoc = HeadLoc;
			bool bFoundNeck = false;
			for (const FName& Bone : NeckBones)
			{
				if (TryBone(Bone, NeckLoc))
				{
					bFoundNeck = true;
					break;
				}
			}
			if (bFoundNeck)
			{
				// Bias toward head (0.62) so we stay on the face, not upper chest.
				OutHead = FMath::Lerp(NeckLoc, HeadLoc, 0.62);
			}
			else
			{
				OutHead = HeadLoc;
				OutHead.Z -= 8.0;
			}
			// Push onto the front of the face mesh (void muzzles / grills stick forward).
			OutHead += Actor->GetActorForwardVector() * 8.0;
			return true;
		}
		return false;
	}

	/** Bounds center (full body). Used for wides / props. */
	FVector ActorBodyCenter(const AActor* Actor)
	{
		if (!Actor)
		{
			return FVector::ZeroVector;
		}
		const FBox Bounds = Actor->GetComponentsBoundingBox(true);
		return Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
	}

	/** Point on the actor's vertical bounds: 0 = feet, 1 = top of mesh. */
	FVector BoundsHeightPoint(const AActor* Actor, double Height01)
	{
		if (!Actor)
		{
			return FVector::ZeroVector;
		}
		const FBox Bounds = Actor->GetComponentsBoundingBox(true);
		if (!Bounds.IsValid)
		{
			return Actor->GetActorLocation();
		}
		const FVector C = Bounds.GetCenter();
		return FVector(C.X, C.Y, FMath::Lerp(Bounds.Min.Z, Bounds.Max.Z, (float)Height01));
	}

	/**
	 * Shot size that actually drives framing. Zoom-in / focus-on without an
	 * explicit size still want the face, not the belly of the bounds.
	 */
	ECineShotSize EffectiveShotSize(const FCineShotSegment& Seg)
	{
		if (Seg.ShotSize != ECineShotSize::Unspecified)
		{
			return Seg.ShotSize;
		}
		if (Seg.Move == ECineMoveType::ZoomIn)
		{
			return ECineShotSize::MediumCloseUp;
		}
		if (Seg.bTrackFocus)
		{
			return ECineShotSize::MediumCloseUp;
		}
		return ECineShotSize::Unspecified;
	}

	/**
	 * Interest point + framing radius for a subject.
	 * Tight shots / characters with a head bone aim at the face; wides stay body-centered.
	 */
	struct FSubjectFraming
	{
		FVector Point = FVector::ZeroVector;
		double Radius = 100.0;
		bool bHead = false;
	};

	/**
	 * Always mid-face when a head exists. Used for follow/aim so tracking never
	 * drifts to hips / body mass while the character animates.
	 */
	FVector ResolveFaceInterestPoint(const AActor* Actor)
	{
		if (!Actor)
		{
			return FVector::ZeroVector;
		}
		FVector Face = FVector::ZeroVector;
		if (FindHeadWorldLocation(Actor, Face))
		{
			return Face;
		}
		// No head bone: upper-third of bounds (chest/face band), not body center.
		return BoundsHeightPoint(Actor, 0.88);
	}

	FSubjectFraming ResolveSubjectFraming(const AActor* Actor, ECineShotSize Size)
	{
		FSubjectFraming Out;
		if (!Actor)
		{
			return Out;
		}

		const FBox Bounds = Actor->GetComponentsBoundingBox(true);
		const FVector BodyCenter = Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
		const double BodyRadius = Bounds.IsValid
			? FMath::Max<double>(Bounds.GetExtent().Size(), 25.0)
			: 100.0;

		FVector HeadLoc = BodyCenter;
		const bool bHasHead = FindHeadWorldLocation(Actor, HeadLoc);
		Out.bHead = bHasHead;
		// Face interest is always the mid-face point when available.
		const FVector FacePoint = bHasHead ? HeadLoc : BoundsHeightPoint(Actor, 0.88);

		switch (Size)
		{
		case ECineShotSize::ExtremeCloseUp:
			// Whole face tight (eyes through chin/grills) — not forehead crop.
			Out.Point = FacePoint;
			Out.Radius = bHasHead
				? FMath::Clamp(BodyRadius * 0.22, 16.0, 40.0)
				: BodyRadius * 0.22;
			break;

		case ECineShotSize::CloseUp:
			// Full face + a little headroom/chin room (void grills, glasses, etc.).
			Out.Point = FacePoint;
			Out.Radius = bHasHead
				? FMath::Clamp(BodyRadius * 0.34, 26.0, 62.0)
				: BodyRadius * 0.30;
			break;

		case ECineShotSize::MediumCloseUp:
			// Chest-up distance, but still aim at the face (not sternum).
			Out.Point = FacePoint;
			if (bHasHead)
			{
				// Tiny drop so shoulders can read without losing the face lock.
				Out.Point.Z -= FMath::Clamp(BodyRadius * 0.03, 3.0, 12.0);
			}
			Out.Radius = BodyRadius * 0.42;
			break;

		case ECineShotSize::Medium:
			// Still face-biased so follow doesn't sit on the belly.
			Out.Point = bHasHead
				? FMath::Lerp(BodyCenter, FacePoint, 0.85)
				: BoundsHeightPoint(Actor, 0.70);
			Out.Radius = BodyRadius * 0.72;
			break;

		case ECineShotSize::Unspecified:
			// Default character framing: face lock, not pelvis.
			Out.Point = FacePoint;
			Out.Radius = bHasHead ? BodyRadius * 0.55 : BodyRadius;
			break;

		case ECineShotSize::Wide:
		case ECineShotSize::ExtremeWide:
		default:
			Out.Point = BodyCenter;
			Out.Radius = BodyRadius;
			break;
		}

		return Out;
	}

	/** Back-compat helper used by rack-focus distance keys. */
	FVector ActorCenter(const AActor* Actor, ECineShotSize Size = ECineShotSize::Unspecified)
	{
		return ResolveSubjectFraming(Actor, Size).Point;
	}

	/** Where the camera lives relative to the subject, before the move is applied. */
	struct FShotGeometry
	{
		bool bHasTarget = false;
		FVector TargetPoint = FVector::ZeroVector;
		double Radius = 100.0;
		double Distance = 500.0;
		/** World-space yaw of (camera - target). */
		double AzimuthDeg = 0.0;
		double ElevationDeg = 0.0;

		/** Where the lens points. Defaults to TargetPoint; a "looking at" actor overrides it. */
		bool bHasLookAt = false;
		FVector AimPoint = FVector::ZeroVector;
	};

	/** Camera distance as a multiple of the subject's framing radius. */
	double FramingFactor(ECineShotSize Size)
	{
		switch (Size)
		{
		case ECineShotSize::ExtremeCloseUp: return 2.6;
		case ECineShotSize::CloseUp:        return 3.3;
		case ECineShotSize::MediumCloseUp:  return 3.6;
		case ECineShotSize::Medium:         return 4.5;
		case ECineShotSize::Wide:           return 8.0;
		case ECineShotSize::ExtremeWide:    return 15.0;
		default:                            return 4.5;
		}
	}

	/** Unit vector at the given yaw/pitch angles (degrees), Z-up. */
	FVector SphericalOffset(double AzimuthDeg, double ElevationDeg)
	{
		const double Az = FMath::DegreesToRadians(AzimuthDeg);
		const double El = FMath::DegreesToRadians(ElevationDeg);
		return FVector(FMath::Cos(El) * FMath::Cos(Az), FMath::Cos(El) * FMath::Sin(Az), FMath::Sin(El));
	}

	FShotGeometry ComputeGeometry(const FCineShotSegment& Seg, const FVector& ViewLoc, const FRotator& ViewRot)
	{
		FShotGeometry Geo;
		const ECineShotSize Size = EffectiveShotSize(Seg);

		if (AActor* Target = Seg.TargetActor.Get())
		{
			const FSubjectFraming Frame = ResolveSubjectFraming(Target, Size);
			Geo.bHasTarget = true;
			Geo.TargetPoint = Frame.Point;
			Geo.Radius = Frame.Radius;

			// Two frames of reference for sides:
			//  - Possessive ("its left") uses the actor's own root rotation — right
			//    for characters, whose facing is usually authored correctly.
			//  - Plain ("from the left") is viewer-relative: "front" is the side of
			//    the actor facing the editor viewport right now, left/right are
			//    screen left/right — predictable for props whose root rotation is
			//    arbitrary.
			// "Left" swings the azimuth opposite ways because the viewer looks
			// toward the subject while the actor looks away from its own front.
			double FacingYaw;
			double LeftSwingDeg;
			if (Seg.bActorRelativeSide)
			{
				FacingYaw = Target->GetActorRotation().Yaw;
				LeftSwingDeg = -90.0;
			}
			else
			{
				const FVector ToViewer = ViewLoc - Geo.TargetPoint;
				FacingYaw = FMath::RadiansToDegrees(FMath::Atan2(ToViewer.Y, ToViewer.X));
				LeftSwingDeg = 90.0;
			}
			switch (Seg.ViewSide)
			{
			case ECineViewSide::Front:        Geo.AzimuthDeg = FacingYaw; break;
			case ECineViewSide::Behind:       Geo.AzimuthDeg = FacingYaw + 180.0; break;
			case ECineViewSide::Left:         Geo.AzimuthDeg = FacingYaw + LeftSwingDeg; break;
			case ECineViewSide::Right:        Geo.AzimuthDeg = FacingYaw - LeftSwingDeg; break;
			case ECineViewSide::OverShoulder: Geo.AzimuthDeg = FacingYaw + 145.0; break;
			}

			switch (Seg.Angle)
			{
			case ECineAngle::Low:      Geo.ElevationDeg = -18.0; break;
			case ECineAngle::High:     Geo.ElevationDeg = 30.0; break;
			case ECineAngle::Overhead: Geo.ElevationDeg = 75.0; break;
			default: break;
			}

			// A longer lens needs to sit further back to hold the same framing.
			const double LensScale = Seg.FocalLengthMm > 0.0f ? Seg.FocalLengthMm / 35.0 : 1.0;
			Geo.Distance = FMath::Max(Geo.Radius * FramingFactor(Size) * LensScale, 40.0);

			if (Seg.ViewSide == ECineViewSide::OverShoulder)
			{
				Geo.Distance *= 0.7;
				Geo.ElevationDeg += 8.0;
			}
		}
		else
		{
			// No subject: anchor the shot on the point the viewport camera is looking at.
			Geo.TargetPoint = ViewLoc + ViewRot.Vector() * 500.0;
			Geo.Radius = 100.0;
			Geo.Distance = 500.0;

			const FVector Offset = ViewLoc - Geo.TargetPoint;
			Geo.AzimuthDeg = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
			Geo.ElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Offset.Z / FMath::Max(Offset.Size(), 1.0), -1.0, 1.0)));
		}

		Geo.AimPoint = Geo.TargetPoint;
		if (AActor* LookAt = Seg.LookAtActor.Get())
		{
			// Aim at the look-at actor with the same framing intent (face on CU, etc.).
			Geo.AimPoint = ResolveSubjectFraming(LookAt, Size).Point;
			Geo.bHasLookAt = true;
		}

		return Geo;
	}

	/**
	 * Geometry for a continuous take: the camera stays where the previous move
	 * left it, and the spherical frame (azimuth/elevation/distance) is derived
	 * from that offset so the next move continues seamlessly. Framing and
	 * view-side words only position the very first move of a take — but the
	 * aim/interest point still updates to the head on close-ups so push-ins
	 * land on the face, not the torso.
	 */
	FShotGeometry ComputeGeometryChained(const FCineShotSegment& Seg, const FVector& PrevPos, const FRotator& PrevRot)
	{
		FShotGeometry Geo;
		const ECineShotSize Size = EffectiveShotSize(Seg);

		if (AActor* Target = Seg.TargetActor.Get())
		{
			const FSubjectFraming Frame = ResolveSubjectFraming(Target, Size);
			Geo.bHasTarget = true;
			Geo.TargetPoint = Frame.Point;
			Geo.Radius = Frame.Radius;
		}
		else
		{
			Geo.TargetPoint = PrevPos + PrevRot.Vector() * 500.0;
			Geo.Radius = 100.0;
		}

		const FVector Offset = PrevPos - Geo.TargetPoint;
		Geo.Distance = FMath::Max(Offset.Size(), 1.0);
		Geo.AzimuthDeg = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
		Geo.ElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Offset.Z / Geo.Distance, -1.0, 1.0)));

		Geo.AimPoint = Geo.TargetPoint;
		if (AActor* LookAt = Seg.LookAtActor.Get())
		{
			Geo.AimPoint = ResolveSubjectFraming(LookAt, Size).Point;
			Geo.bHasLookAt = true;
		}

		return Geo;
	}

	/** Offset from actor origin so tracking focus locks on the face (not body mass). */
	FVector TrackingFocusOffset(const AActor* Actor, ECineShotSize /*Size*/)
	{
		if (!Actor)
		{
			return FVector::ZeroVector;
		}
		return ResolveFaceInterestPoint(Actor) - Actor->GetActorLocation();
	}

	/** Prefer explicit look-at subject, else the shot's pivot target. */
	AActor* ResolveFocusActor(const FCineShotSegment& Seg)
	{
		if (AActor* Look = Seg.LookAtActor.Get())
		{
			return Look;
		}
		return Seg.TargetActor.Get();
	}

	/** World-space interest point DOF should hit for this segment (always face when possible). */
	FVector ResolveFocusPoint(const FCineShotSegment& Seg, const FShotGeometry& Geo)
	{
		if (AActor* FocusActor = ResolveFocusActor(Seg))
		{
			return ResolveFaceInterestPoint(FocusActor);
		}
		if (Geo.bHasLookAt || Geo.bHasTarget)
		{
			return Geo.AimPoint;
		}
		return Geo.TargetPoint;
	}

	void ApplyTrackingFocus(UCineCameraComponent* Lens, AActor* FocusActor, ECineShotSize Size)
	{
		if (!Lens || !FocusActor)
		{
			return;
		}
		Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
		Lens->FocusSettings.TrackingFocusSettings.ActorToTrack = FocusActor;
		Lens->FocusSettings.TrackingFocusSettings.RelativeOffset = TrackingFocusOffset(FocusActor, Size);
		Lens->FocusSettings.bSmoothFocusChanges = false;
		Lens->FocusSettings.FocusOffset = 0.0f;
	}

	void ApplyDeepFocus(UCineCameraComponent* Lens)
	{
		if (!Lens)
		{
			return;
		}
		// Far manual plane — full stage stays sharp without disabling DOF entirely.
		Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		Lens->FocusSettings.ManualFocusDistance = 100000.0f;
		Lens->FocusSettings.bSmoothFocusChanges = false;
		Lens->FocusSettings.FocusOffset = 0.0f;
	}

	void ApplyFixedFocus(UCineCameraComponent* Lens, float DistanceCm)
	{
		if (!Lens)
		{
			return;
		}
		Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		Lens->FocusSettings.ManualFocusDistance = FMath::Max(DistanceCm, 10.0f);
		Lens->FocusSettings.bSmoothFocusChanges = false;
		Lens->FocusSettings.FocusOffset = 0.0f;
	}

	/** PP effects + color grade + optional scope/IMAX filmback from a segment look. */
	void ApplyCameraLook(UCineCameraComponent* Lens, const FCineShotSegment& Seg)
	{
		if (!Lens)
		{
			return;
		}

		FPostProcessSettings& PP = Lens->PostProcessSettings;
		if (Seg.FilmGrainIntensity > 0.0f)
		{
			PP.bOverride_FilmGrainIntensity = true;
			PP.FilmGrainIntensity = Seg.FilmGrainIntensity;
		}
		if (Seg.VignetteIntensity > 0.0f)
		{
			PP.bOverride_VignetteIntensity = true;
			PP.VignetteIntensity = Seg.VignetteIntensity;
		}
		if (Seg.ChromaticAberrationIntensity > 0.0f)
		{
			PP.bOverride_SceneFringeIntensity = true;
			PP.SceneFringeIntensity = Seg.ChromaticAberrationIntensity;
		}
		if (Seg.BloomIntensity > 0.0f)
		{
			PP.bOverride_BloomIntensity = true;
			PP.BloomIntensity = Seg.BloomIntensity;
		}
		if (Seg.LensFlareIntensity > 0.0f)
		{
			PP.bOverride_LensFlareIntensity = true;
			PP.LensFlareIntensity = Seg.LensFlareIntensity;
		}

		if (Seg.bApplyLookGrade)
		{
			const float Sat = FMath::Clamp(Seg.LookSaturation, 0.0f, 2.0f);
			const float Con = FMath::Clamp(Seg.LookContrast, 0.2f, 2.0f);
			const float Gain = FMath::Clamp(Seg.LookGain, 0.2f, 2.0f);

			PP.bOverride_ColorSaturation = true;
			PP.ColorSaturation = FVector4(Sat, Sat, Sat, 1.0f);

			PP.bOverride_ColorContrast = true;
			PP.ColorContrast = FVector4(Con, Con, Con, 1.0f);

			PP.bOverride_ColorGain = true;
			PP.ColorGain = FVector4(Gain, Gain, Gain, 1.0f);

			if (Seg.WhiteTempKelvin > 0.0f)
			{
				PP.bOverride_WhiteTemp = true;
				PP.WhiteTemp = FMath::Clamp(Seg.WhiteTempKelvin, 1500.0f, 15000.0f);
			}
			if (!FMath::IsNearlyZero(Seg.WhiteTint))
			{
				PP.bOverride_WhiteTint = true;
				PP.WhiteTint = FMath::Clamp(Seg.WhiteTint, -1.0f, 1.0f);
			}
			if (!Seg.SceneColorTint.Equals(FLinearColor::White, 0.001f))
			{
				PP.bOverride_SceneColorTint = true;
				PP.SceneColorTint = Seg.SceneColorTint;
			}
		}

		if (Seg.MotionBlurAmount >= 0.0f)
		{
			PP.bOverride_MotionBlurAmount = true;
			PP.MotionBlurAmount = FMath::Clamp(Seg.MotionBlurAmount, 0.0f, 1.0f);
		}

		if (Seg.FilmbackSensorHeightMm > 0.0f && Seg.FilmbackSensorWidthMm > 0.0f)
		{
			FCameraFilmbackSettings Filmback = Lens->Filmback;
			Filmback.SensorWidth = Seg.FilmbackSensorWidthMm;
			Filmback.SensorHeight = Seg.FilmbackSensorHeightMm;
			Lens->SetFilmback(Filmback);
		}
	}

	/** Merge look fields across continuous-take moves (max PP, first grade/filmback). */
	void MergeLookInto(FCineShotSegment& Dest, const FCineShotSegment& Src)
	{
		Dest.FilmGrainIntensity = FMath::Max(Dest.FilmGrainIntensity, Src.FilmGrainIntensity);
		Dest.VignetteIntensity = FMath::Max(Dest.VignetteIntensity, Src.VignetteIntensity);
		Dest.ChromaticAberrationIntensity = FMath::Max(Dest.ChromaticAberrationIntensity, Src.ChromaticAberrationIntensity);
		Dest.BloomIntensity = FMath::Max(Dest.BloomIntensity, Src.BloomIntensity);
		Dest.LensFlareIntensity = FMath::Max(Dest.LensFlareIntensity, Src.LensFlareIntensity);
		if (!Src.StyleKitName.IsEmpty())
		{
			if (Dest.StyleKitName.IsEmpty())
			{
				Dest.StyleKitName = Src.StyleKitName;
			}
			else if (!Dest.StyleKitName.Contains(Src.StyleKitName))
			{
				Dest.StyleKitName += TEXT(" + ") + Src.StyleKitName;
			}
		}
		if (Src.bApplyLookGrade && !Dest.bApplyLookGrade)
		{
			Dest.bApplyLookGrade = true;
			Dest.LookSaturation = Src.LookSaturation;
			Dest.LookContrast = Src.LookContrast;
			Dest.LookGain = Src.LookGain;
			Dest.WhiteTempKelvin = Src.WhiteTempKelvin;
			Dest.WhiteTint = Src.WhiteTint;
			Dest.SceneColorTint = Src.SceneColorTint;
		}
		if (Dest.MotionBlurAmount < 0.0f && Src.MotionBlurAmount >= 0.0f)
		{
			Dest.MotionBlurAmount = Src.MotionBlurAmount;
		}
		if (Dest.FilmbackSensorHeightMm <= 0.0f && Src.FilmbackSensorHeightMm > 0.0f)
		{
			Dest.FilmbackSensorWidthMm = Src.FilmbackSensorWidthMm;
			Dest.FilmbackSensorHeightMm = Src.FilmbackSensorHeightMm;
		}
	}

	/**
	 * Evaluates a segment's camera transform at normalized time.
	 * TMove is the eased move parameter; TReal is wall-clock progress (handheld
	 * noise runs on real time so easing doesn't stretch the shake).
	 */
	struct FShotSampler
	{
		const FCineShotSegment& Seg;
		const FShotGeometry Geo;

		FVector StartPos = FVector::ZeroVector;
		FRotator StartRot = FRotator::ZeroRotator;
		FVector FwdVec = FVector::ForwardVector;
		FVector RightVec = FVector::RightVector;
		bool bAim = false;
		bool bIsOrbit = false;
		bool bIsPanTilt = false;
		/** Continuous-take mode: this segment starts where the previous one ended. */
		bool bChained = false;

		FShotSampler(const FCineShotSegment& InSeg, const FShotGeometry& InGeo, const FRotator& ViewRot, bool bInChained = false)
			: Seg(InSeg)
			, Geo(InGeo)
			, bChained(bInChained)
		{
			bIsOrbit = Seg.Move == ECineMoveType::OrbitCW || Seg.Move == ECineMoveType::OrbitCCW;
			bIsPanTilt =
				Seg.Move == ECineMoveType::PanLeft || Seg.Move == ECineMoveType::PanRight ||
				Seg.Move == ECineMoveType::TiltUp || Seg.Move == ECineMoveType::TiltDown;

			StartPos = Geo.TargetPoint + SphericalOffset(Geo.AzimuthDeg, Geo.ElevationDeg) * Geo.Distance;
			StartRot = (Geo.bHasTarget || Geo.bHasLookAt) ? (Geo.AimPoint - StartPos).Rotation() : ViewRot;

			// Orbits always pivot around the target point, even the target-less
			// viewport-anchored kind, or the move would read as a weird strafe.
			// An explicit "looking at" actor always keeps the lens aimed.
			bAim = !bIsPanTilt &&
				(bIsOrbit ||
				 Geo.bHasLookAt ||
				 (Geo.bHasTarget && Seg.bLookAtTarget) ||
				 (Seg.Move == ECineMoveType::Flyover && Geo.bHasTarget));

			// Follow subject translation whenever we have a named target / look-at
			// (body performance moves them out of a static bake otherwise).
			// Continuous + multi-cut both rely on this; the parser also defaults
			// bFollowSubjectPosition for any named character.
			bFollowSubject = (Geo.bHasTarget && Seg.TargetActor.IsValid())
				|| (Geo.bHasLookAt && Seg.LookAtActor.IsValid())
				|| Seg.bFollowSubjectPosition;

			FwdVec = StartRot.Vector();
			RightVec = FRotationMatrix(StartRot).GetUnitAxis(EAxis::Y);
			// Camera offset from the interest point at t=0 — reapplied to the live face each sample.
			RelOffsetFromTarget = StartPos - Geo.TargetPoint;
			HoldDistance = Geo.Distance;
			HoldElevationDeg = Geo.ElevationDeg;

			// Face-front relative: store azimuth relative to the actor's facing so
			// "from its front" stays on the face as they turn (not a world-fixed side).
			if (AActor* SpaceActor = Seg.TargetActor.IsValid() ? Seg.TargetActor.Get() : Seg.LookAtActor.Get())
			{
				SetupActorYaw = SpaceActor->GetActorRotation().Yaw;
				RelAzimuthToFacing = Geo.AzimuthDeg - SetupActorYaw;
				// Prefer face-relative spherical for any subject follow (holds + orbits).
				bFaceRelative = Seg.bFollowSubjectPosition || Seg.bActorRelativeSide
					|| Seg.ViewSide == ECineViewSide::Front;
			}
		}

		bool bFollowSubject = false;
		/** Rebuild camera on a sphere around the live face, relative to actor yaw. */
		bool bFaceRelative = false;
		FVector RelOffsetFromTarget = FVector::ZeroVector;
		double RelAzimuthToFacing = 0.0;
		double SetupActorYaw = 0.0;
		double HoldDistance = 100.0;
		double HoldElevationDeg = 0.0;

		/** Live actor used for face-relative yaw (target preferred). */
		AActor* SpaceActor() const
		{
			if (Seg.TargetActor.IsValid())
			{
				return Seg.TargetActor.Get();
			}
			return Seg.LookAtActor.Get();
		}

		/** World azimuth that keeps the same side of the face as at setup. */
		double LiveAzimuth(double OrbitDeltaDeg = 0.0) const
		{
			if (bFaceRelative)
			{
				if (AActor* A = SpaceActor())
				{
					return A->GetActorRotation().Yaw + RelAzimuthToFacing + OrbitDeltaDeg;
				}
			}
			return Geo.AzimuthDeg + OrbitDeltaDeg;
		}

		/**
		 * @param LiveTarget  Animated face interest. Falls back to Geo.TargetPoint.
		 * @param LiveAim     Animated aim (face). Falls back to Geo.AimPoint / LiveTarget.
		 */
		void Sample(double TMove, double TReal, FVector& OutPos, FRotator& OutRot,
			const FVector* LiveTarget = nullptr, const FVector* LiveAim = nullptr) const
		{
			// Always pivot/aim on the live face when following — never body mass.
			FVector Target = LiveTarget ? *LiveTarget : Geo.TargetPoint;
			FVector Aim = LiveAim ? *LiveAim : (Geo.bHasLookAt ? Geo.AimPoint : Target);
			if (bFollowSubject)
			{
				if (AActor* FaceActor = SpaceActor())
				{
					const FVector Face = ResolveFaceInterestPoint(FaceActor);
					if (!Face.IsNearlyZero())
					{
						Target = Face;
						// Aim at face unless an explicit different look-at actor is set.
						if (!Seg.LookAtActor.IsValid() || Seg.LookAtActor == Seg.TargetActor)
						{
							Aim = Face;
						}
					}
				}
				if (Seg.LookAtActor.IsValid() && Seg.LookAtActor != Seg.TargetActor)
				{
					Aim = ResolveFaceInterestPoint(Seg.LookAtActor.Get());
				}
			}

			const double Amount = Seg.MoveAmount;
			double Dist = Geo.Distance;
			const double El = Geo.ElevationDeg;
			FVector Pos = StartPos;

			switch (Seg.Move)
			{
			case ECineMoveType::Static:
			case ECineMoveType::ZoomIn:   // zooms move the lens, not the camera
			case ECineMoveType::ZoomOut:
				// Hold on the front/side of the live face (spherical, actor-yaw relative).
				Pos = Target + SphericalOffset(LiveAzimuth(), HoldElevationDeg) * HoldDistance;
				break;

			case ECineMoveType::DollyIn:
			{
				const double D = Amount > 0.0 ? Amount : Dist * 0.5;
				Dist = FMath::Max(Dist - D * TMove, Geo.Radius * 1.05);
				Pos = Target + SphericalOffset(LiveAzimuth(), El) * Dist;
				break;
			}
			case ECineMoveType::DollyOut:
			{
				const double D = Amount > 0.0 ? Amount : Geo.Distance;
				Pos = Target + SphericalOffset(LiveAzimuth(), El) * (Geo.Distance + D * TMove);
				break;
			}
			case ECineMoveType::OrbitCW:
			case ECineMoveType::OrbitCCW:
			{
				const double Deg = Amount > 0.0 ? Amount : 90.0;
				const double OrbitDelta = (Seg.Move == ECineMoveType::OrbitCW ? Deg : -Deg) * TMove;
				Pos = Target + SphericalOffset(LiveAzimuth(OrbitDelta), El) * Dist;
				break;
			}
			case ECineMoveType::TruckLeft:
			case ECineMoveType::TruckRight:
			{
				// Lateral move around the face while staying face-locked.
				const double D = Amount > 0.0 ? Amount : FMath::Max(Dist * 0.75, 300.0);
				const double SideSign = (Seg.Move == ECineMoveType::TruckRight) ? 1.0 : -1.0;
				// Convert truck distance into a small yaw arc on the face sphere.
				const double ArcDeg = FMath::RadiansToDegrees(D / FMath::Max(HoldDistance, 1.0)) * SideSign * TMove;
				Pos = Target + SphericalOffset(LiveAzimuth(ArcDeg), HoldElevationDeg) * HoldDistance;
				break;
			}
			case ECineMoveType::CraneUp:
			case ECineMoveType::CraneDown:
			{
				const double D = Amount > 0.0 ? Amount : 300.0;
				const double ElDelta = (Seg.Move == ECineMoveType::CraneUp ? 1.0 : -1.0)
					* FMath::RadiansToDegrees(D / FMath::Max(HoldDistance, 1.0)) * TMove;
				Pos = Target + SphericalOffset(LiveAzimuth(), HoldElevationDeg + ElDelta) * HoldDistance;
				break;
			}
			case ECineMoveType::Flyover:
			{
				const double Az = LiveAzimuth();
				if (bChained && Geo.bHasTarget)
				{
					const FVector Start = Target + SphericalOffset(Az, HoldElevationDeg) * HoldDistance;
					const FVector End(
						2.0 * Target.X - Start.X,
						2.0 * Target.Y - Start.Y,
						Start.Z);
					Pos = FMath::Lerp(Start, End, TMove);
					Pos.Z += FMath::Sin(TMove * PI) * FMath::Max(Geo.Radius * 1.5, 300.0);
				}
				else if (Geo.bHasTarget)
				{
					const double Travel = Amount > 0.0 ? Amount : FMath::Max(Dist * 2.0, 3000.0);
					const double Height = FMath::Max(Geo.Radius * 2.5, 500.0);
					const FVector AzDir = SphericalOffset(Az, 0.0);
					const FVector PathStart = Target + AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
					const FVector PathEnd = Target - AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
					Pos = FMath::Lerp(PathStart, PathEnd, TMove);
				}
				else
				{
					const double Travel = Amount > 0.0 ? Amount : 3000.0;
					Pos = StartPos + FwdVec * Travel * TMove;
				}
				break;
			}
			case ECineMoveType::PanLeft:
			case ECineMoveType::PanRight:
			case ECineMoveType::TiltUp:
			case ECineMoveType::TiltDown:
				// Stay on the face while panning/tilting the lens.
				Pos = Target + SphericalOffset(LiveAzimuth(), HoldElevationDeg) * HoldDistance;
				break;
			}

			FRotator Rot = StartRot;
			if (bIsPanTilt)
			{
				// Base aim on the live face, then apply the pan/tilt offset.
				Rot = (Aim - Pos).Rotation();
				const bool bIsPan = Seg.Move == ECineMoveType::PanLeft || Seg.Move == ECineMoveType::PanRight;
				const double Deg = (Amount > 0.0 ? Amount : (bIsPan ? 45.0 : 25.0)) * TMove;
				switch (Seg.Move)
				{
				case ECineMoveType::PanLeft:  Rot.Yaw -= Deg; break;
				case ECineMoveType::PanRight: Rot.Yaw += Deg; break;
				case ECineMoveType::TiltUp:   Rot.Pitch += Deg; break;
				case ECineMoveType::TiltDown: Rot.Pitch -= Deg; break;
				default: break;
				}
			}
			else if (bAim || bFollowSubject)
			{
				// Always re-aim at the live face when following a moving subject.
				Rot = (Aim - Pos).Rotation();
			}

			if (Seg.HandheldIntensity > 0.0f)
			{
				const double Time = TReal * Seg.DurationSeconds;
				auto Noise = [Time](int32 Seed)
				{
					return (double)FMath::PerlinNoise1D((float)(Time * 1.3) + Seed * 37.77f);
				};
				Pos += FVector(Noise(1), Noise(2), Noise(3)) * (Seg.HandheldIntensity * 4.0);
				Rot.Pitch += Noise(4) * Seg.HandheldIntensity * 0.9;
				Rot.Yaw += Noise(5) * Seg.HandheldIntensity * 0.9;
				Rot.Roll += Noise(6) * Seg.HandheldIntensity * 0.5;
			}

			Rot.Roll += Seg.DutchAngleDeg;

			OutPos = Pos;
			OutRot = Rot;
		}

		/** Curved paths, noise, and subject-follow can't be represented by two keys. */
		bool NeedsDenseKeys() const
		{
			if (bFollowSubject)
			{
				return true;
			}
			if (Seg.HandheldIntensity > 0.0f)
			{
				return true;
			}
			if (bIsOrbit || Seg.Move == ECineMoveType::Flyover)
			{
				return true;
			}
			// A straight-line move that keeps re-aiming at the target bends the rotation curve.
			const bool bLinearMove =
				Seg.Move == ECineMoveType::TruckLeft || Seg.Move == ECineMoveType::TruckRight ||
				Seg.Move == ECineMoveType::CraneUp || Seg.Move == ECineMoveType::CraneDown;
			return bAim && bLinearMove;
		}
	};

	/**
	 * Scrub the open sequence so skeletal poses (body performance) match a time.
	 * UE 5.4+ requires FMovieSceneSequencePlaybackParams — the old SetCurrentLocalTime(int)
	 * took display frames, so passing seconds silently scrubbed to the wrong pose and
	 * "follow" only re-aimed at a frozen world point.
	 */
	void EvaluateSequenceAtSeconds(ULevelSequence* Sequence, double TimeSeconds)
	{
		if (!Sequence)
		{
			return;
		}
		const FMovieSceneSequencePlaybackParams Params(
			static_cast<float>(FMath::Max(TimeSeconds, 0.0)),
			EUpdatePositionMethod::Jump);
		ULevelSequenceEditorBlueprintLibrary::SetLocalPosition(Params, EMovieSceneTimeUnit::DisplayRate);
	}

	/** Current local playhead time in seconds (for save/restore around follow bakes). */
	float GetSequenceLocalTimeSeconds()
	{
		const FMovieSceneSequencePlaybackParams Pos =
			ULevelSequenceEditorBlueprintLibrary::GetLocalPosition(EMovieSceneTimeUnit::DisplayRate);
		if (Pos.PositionType == EMovieScenePositionType::Time)
		{
			return Pos.Time;
		}
		// Frame-based return: convert display frames → seconds via the open sequence rate.
		if (ULevelSequence* Seq = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence())
		{
			if (UMovieScene* MS = Seq->GetMovieScene())
			{
				const FFrameRate DisplayRate = MS->GetDisplayRate();
				return static_cast<float>(DisplayRate.AsSeconds(Pos.Frame));
			}
		}
		return Pos.Time;
	}

	void RestoreSequenceLocalTimeSeconds(float TimeSeconds)
	{
		const FMovieSceneSequencePlaybackParams Params(TimeSeconds, EUpdatePositionMethod::Jump);
		ULevelSequenceEditorBlueprintLibrary::SetLocalPosition(Params, EMovieSceneTimeUnit::DisplayRate);
	}

	/** Refresh bone caches after a sequencer scrub so GetBoneLocation is current. */
	void RefreshActorBones(AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}
		TArray<USkeletalMeshComponent*> Meshes;
		Actor->GetComponents<USkeletalMeshComponent>(Meshes);
		for (USkeletalMeshComponent* Skel : Meshes)
		{
			if (!Skel)
			{
				continue;
			}
			Skel->TickAnimation(0.0f, false);
			Skel->RefreshBoneTransforms();
			Skel->UpdateComponentToWorld();
		}
	}

	/** Live face / aim points for a segment after evaluating the sequence. */
	void ResolveLiveFraming(const FCineShotSegment& Seg, FVector& OutTarget, FVector& OutAim)
	{
		OutTarget = FVector::ZeroVector;
		OutAim = FVector::ZeroVector;

		// Always resolve mid-face for follow/aim — shot size only affects distance (radius),
		// not which body part we lock onto.
		if (AActor* Target = Seg.TargetActor.Get())
		{
			RefreshActorBones(Target);
			OutTarget = ResolveFaceInterestPoint(Target);
			OutAim = OutTarget;
		}
		if (AActor* Look = Seg.LookAtActor.Get())
		{
			RefreshActorBones(Look);
			OutAim = ResolveFaceInterestPoint(Look);
			if (OutTarget.IsNearlyZero())
			{
				OutTarget = OutAim;
			}
		}
	}

	/** Euler state carried across key batches so rotation channels never snap through 0/360. */
	struct FEulerContinuity
	{
		bool bValid = false;
		double Roll = 0.0;
		double Pitch = 0.0;
		double Yaw = 0.0;
	};

	/**
	 * What the level's sun should look like for a time-of-day word.
	 *
	 * Two flavors: the legacy values fake the mood entirely through the light
	 * (absolute intensity, tinted color) for levels with no SkyAtmosphere. The
	 * physical values assume an atmosphere is doing the coloring — only the
	 * pitch and a multiplier on the level's own sun brightness are keyed, so
	 * lux-scale setups keep their exposure; color stays white except for the
	 * moonlight tint, which no atmosphere derives.
	 */
	struct FSunPreset
	{
		/** Overcast keeps the level's sun angle and only flattens color/intensity. */
		bool bSetPitch = true;
		double PitchDeg = -45.0;
		FLinearColor Color = FLinearColor::White;
		float Intensity = 10.0f;
		float PhysicalMultiplier = 1.0f;
		FLinearColor PhysicalColor = FLinearColor::White;
	};

	bool GetSunPreset(ECineTimeOfDay TimeOfDay, FSunPreset& Out)
	{
		const FLinearColor White = FLinearColor::White;
		switch (TimeOfDay)
		{
		case ECineTimeOfDay::Dawn:       Out = { true,  -6.0, FLinearColor(1.00f, 0.62f, 0.38f), 4.0f,  0.80f, White }; return true;
		case ECineTimeOfDay::Morning:    Out = { true, -30.0, FLinearColor(1.00f, 0.93f, 0.82f), 8.0f,  1.00f, White }; return true;
		case ECineTimeOfDay::Noon:       Out = { true, -75.0, FLinearColor(1.00f, 1.00f, 1.00f), 10.0f, 1.00f, White }; return true;
		case ECineTimeOfDay::Afternoon:  Out = { true, -45.0, FLinearColor(1.00f, 0.96f, 0.88f), 9.0f,  1.00f, White }; return true;
		case ECineTimeOfDay::GoldenHour: Out = { true,  -9.0, FLinearColor(1.00f, 0.68f, 0.32f), 5.0f,  0.90f, White }; return true;
		case ECineTimeOfDay::Sunset:     Out = { true,  -3.0, FLinearColor(1.00f, 0.45f, 0.18f), 3.0f,  0.80f, White }; return true;
		// Sun just below the horizon: the sky/skylight carries the scene.
		case ECineTimeOfDay::Dusk:       Out = { true,   4.0, FLinearColor(0.55f, 0.55f, 0.75f), 1.2f,  0.30f, FLinearColor(0.80f, 0.82f, 0.95f) }; return true;
		// A cool dim "moon" stand-in rather than true darkness.
		case ECineTimeOfDay::Night:      Out = { true, -35.0, FLinearColor(0.45f, 0.55f, 0.90f), 0.35f, 0.02f, FLinearColor(0.70f, 0.80f, 1.00f) }; return true;
		case ECineTimeOfDay::Midnight:   Out = { true, -60.0, FLinearColor(0.40f, 0.48f, 0.85f), 0.15f, 0.01f, FLinearColor(0.65f, 0.75f, 1.00f) }; return true;
		case ECineTimeOfDay::Overcast:   Out = { false,  0.0, FLinearColor(0.82f, 0.86f, 0.95f), 3.0f,  0.35f, FLinearColor(0.90f, 0.93f, 1.00f) }; return true;
		default: return false;
		}
	}

	/**
	 * Reuse an existing possessable of the same name/class (so re-running the tool
	 * doesn't stack duplicate sun/fog bindings), otherwise add and bind a new one.
	 */
	FGuid FindOrAddPossessable(ULevelSequence* Sequence, UMovieScene* MovieScene, const FString& Name, UObject& Object, UObject* Context, const FGuid& ParentGuid = FGuid())
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& Existing = MovieScene->GetPossessable(i);
			if (Existing.GetName() == Name && Object.GetClass()->IsChildOf(Existing.GetPossessedObjectClass()))
			{
				return Existing.GetGuid();
			}
		}

		const FGuid Guid = MovieScene->AddPossessable(Name, Object.GetClass());
		if (ParentGuid.IsValid())
		{
			if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
			{
				Possessable->SetParent(ParentGuid, MovieScene);
			}
		}
		Sequence->BindPossessableObject(Guid, Object, Context);
		return Guid;
	}

	/** First section of the track grown to cover Range, or a fresh one. */
	template <typename SectionType>
	SectionType* GetOrCreateSection(UMovieSceneTrack* Track, const TRange<FFrameNumber>& Range)
	{
		if (Track->GetAllSections().Num() > 0)
		{
			UMovieSceneSection* Section = Track->GetAllSections()[0];
			Section->SetRange(TRange<FFrameNumber>::Hull(Section->GetRange(), Range));
			return Cast<SectionType>(Section);
		}
		SectionType* Section = Cast<SectionType>(Track->CreateNewSection());
		Section->SetRange(Range);
		Track->AddSection(*Section);
		return Section;
	}

	template <typename TrackType>
	TrackType* FindOrAddPropertyTrack(UMovieScene* MovieScene, const FGuid& Guid, FName PropertyName, const FString& PropertyPath)
	{
		TrackType* Track = Cast<TrackType>(MovieScene->FindTrack(TrackType::StaticClass(), Guid, PropertyName));
		if (!Track)
		{
			Track = Cast<TrackType>(MovieScene->AddTrack(TrackType::StaticClass(), Guid));
			Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
		}
		return Track;
	}

	/**
	 * Keys the level's sun (pitch / color / intensity) and height fog density per
	 * shot, using constant-interp keys so lighting snaps at each cut instead of
	 * lerping across shots. Lighting words on any segment bind the light into the
	 * sequence; segments without lighting words simply hold the previous state.
	 */
	void ApplyLightingTracks(ULevelSequence* Sequence, UMovieScene* MovieScene, UWorld* World,
		const TArray<TPair<const FCineShotSegment*, FFrameNumber>>& SegmentStarts,
		const TRange<FFrameNumber>& OverallRange, FCineExecuteResult& Result)
	{
		bool bAnySun = false, bAnyFog = false, bAnyGodRays = false, bAnyVolumetric = false;
		for (const TPair<const FCineShotSegment*, FFrameNumber>& Pair : SegmentStarts)
		{
			bAnySun |= Pair.Key->TimeOfDay != ECineTimeOfDay::Unchanged;
			bAnyFog |= Pair.Key->FogDensity >= 0.0f;
			bAnyGodRays |= Pair.Key->bGodRays;
			bAnyVolumetric |= Pair.Key->bVolumetricFog;
		}
		if (!bAnySun && !bAnyFog && !bAnyGodRays && !bAnyVolumetric)
		{
			return;
		}

		// ---- Sun --------------------------------------------------------------
		// Prefer the directional light the atmosphere already follows; with two
		// suns in a level, keying the other one would change nothing visible.
		ADirectionalLight* Sun = nullptr;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			if (!Sun)
			{
				Sun = *It;
			}
			if ((*It)->GetLightComponent()->IsUsedAsAtmosphereSunLight())
			{
				Sun = *It;
				break;
			}
		}
		if (!Sun && (bAnySun || bAnyGodRays))
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-45.0f, 0.0f, 0.0f), SpawnParams);
			if (Sun)
			{
				Sun->SetActorLabel(TEXT("CineDirector Sun"));
				Result.Notes.Add(TEXT("No directional light in the level — spawned 'CineDirector Sun'."));
			}
		}

		bool bPhysicalSky = false;
		if (Sun && bAnySun)
		{
			// Physical sky: make sure the whole stack exists, spawning what's
			// missing, then let the atmosphere derive sky and sun color from the
			// keyed sun pitch instead of faking it with tinted light.
			ASkyAtmosphere* Atmosphere = nullptr;
			for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
			{
				Atmosphere = *It;
				break;
			}
			if (!Atmosphere)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.ObjectFlags |= RF_Transactional;
				Atmosphere = World->SpawnActor<ASkyAtmosphere>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
				if (Atmosphere)
				{
					Atmosphere->SetActorLabel(TEXT("CineDirector Sky Atmosphere"));
					Result.Notes.Add(TEXT("No SkyAtmosphere in the level — spawned 'CineDirector Sky Atmosphere'."));
				}
			}

			ASkyLight* SkyLight = nullptr;
			for (TActorIterator<ASkyLight> It(World); It; ++It)
			{
				SkyLight = *It;
				break;
			}
			if (!SkyLight)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.ObjectFlags |= RF_Transactional;
				SkyLight = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
				if (SkyLight && SkyLight->GetLightComponent())
				{
					// Real-time capture keeps the ambient light in step with the keyed
					// sun, so night shots go dark without a manual recapture.
					USkyLightComponent* SkyComp = SkyLight->GetLightComponent();
					SkyComp->Modify();
					SkyComp->SetMobility(EComponentMobility::Movable);
					SkyComp->SetRealTimeCaptureEnabled(true);
					SkyLight->SetActorLabel(TEXT("CineDirector Sky Light"));
					Result.Notes.Add(TEXT("No SkyLight in the level — spawned 'CineDirector Sky Light' (real-time capture)."));
				}
			}

			bPhysicalSky = Atmosphere != nullptr;
			if (bPhysicalSky)
			{
				if (UDirectionalLightComponent* DirComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
				{
					if (!DirComp->IsUsedAsAtmosphereSunLight())
					{
						DirComp->Modify();
						DirComp->SetAtmosphereSunLight(true);
						Result.Notes.Add(TEXT("Enabled 'Atmosphere Sun Light' on the sun so the sky follows it."));
					}
				}
			}

			Sun->Modify();
			const FString SunLabel = Sun->GetActorLabel();
			const FGuid SunGuid = FindOrAddPossessable(Sequence, MovieScene, SunLabel, *Sun, World);

			// Pitch drives the sun's elevation; everything else defaults to what the
			// level already had (notably yaw, so the light's composition survives).
			UMovieScene3DTransformTrack* SunTransform = Cast<UMovieScene3DTransformTrack>(MovieScene->FindTrack(UMovieScene3DTransformTrack::StaticClass(), SunGuid));
			if (!SunTransform)
			{
				SunTransform = Cast<UMovieScene3DTransformTrack>(MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), SunGuid));
			}
			UMovieScene3DTransformSection* SunSection = GetOrCreateSection<UMovieScene3DTransformSection>(SunTransform, OverallRange);
			TArrayView<FMovieSceneDoubleChannel*> SunChannels = SunSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
			if (SunChannels.Num() >= 9)
			{
				const FVector SunLoc = Sun->GetActorLocation();
				const FRotator SunRot = Sun->GetActorRotation();
				const double Defaults[9] = { SunLoc.X, SunLoc.Y, SunLoc.Z, SunRot.Roll, SunRot.Pitch, SunRot.Yaw, 1.0, 1.0, 1.0 };
				for (int32 i = 0; i < 9; ++i)
				{
					if (!SunChannels[i]->GetDefault().IsSet())
					{
						SunChannels[i]->SetDefault(Defaults[i]);
					}
				}
			}

			ULightComponent* LightComp = Sun->GetLightComponent();
			const FGuid LightGuid = FindOrAddPossessable(Sequence, MovieScene, LightComp->GetName(), *LightComp, Sun, SunGuid);

			UMovieSceneFloatTrack* IntensityTrack = FindOrAddPropertyTrack<UMovieSceneFloatTrack>(MovieScene, LightGuid, TEXT("Intensity"), TEXT("Intensity"));
			UMovieSceneFloatSection* IntensitySection = GetOrCreateSection<UMovieSceneFloatSection>(IntensityTrack, OverallRange);
			TArrayView<FMovieSceneFloatChannel*> IntensityChannels = IntensitySection->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();

			UMovieSceneColorTrack* ColorTrack = FindOrAddPropertyTrack<UMovieSceneColorTrack>(MovieScene, LightGuid, TEXT("LightColor"), TEXT("LightColor"));
			UMovieSceneColorSection* ColorSection = GetOrCreateSection<UMovieSceneColorSection>(ColorTrack, OverallRange);
			TArrayView<FMovieSceneFloatChannel*> ColorChannels = ColorSection->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();

			// Physical mode scales the level's own sun brightness so lux-scale
			// setups keep their exposure. The pre-animation value lives in the
			// channel default: on re-runs the component may already be showing a
			// keyed value (e.g. 2% for night), which must not become the new base.
			float BaseIntensity = LightComp->Intensity;
			if (IntensityChannels.Num() > 0)
			{
				if (IntensityChannels[0]->GetDefault().IsSet())
				{
					BaseIntensity = IntensityChannels[0]->GetDefault().GetValue();
				}
				else
				{
					IntensityChannels[0]->SetDefault(BaseIntensity);
				}
			}

			for (const TPair<const FCineShotSegment*, FFrameNumber>& Pair : SegmentStarts)
			{
				FSunPreset Preset;
				if (!GetSunPreset(Pair.Key->TimeOfDay, Preset))
				{
					continue;
				}
				const FFrameNumber At = Pair.Value;
				const float Intensity = bPhysicalSky ? BaseIntensity * Preset.PhysicalMultiplier : Preset.Intensity;
				const FLinearColor Color = bPhysicalSky ? Preset.PhysicalColor : Preset.Color;

				if (Preset.bSetPitch && SunChannels.Num() >= 9)
				{
					SunChannels[4]->AddConstantKey(At, Preset.PitchDeg);
				}
				if (IntensityChannels.Num() > 0)
				{
					IntensityChannels[0]->AddConstantKey(At, Intensity);
				}
				if (ColorChannels.Num() >= 4)
				{
					ColorChannels[0]->AddConstantKey(At, Color.R);
					ColorChannels[1]->AddConstantKey(At, Color.G);
					ColorChannels[2]->AddConstantKey(At, Color.B);
					ColorChannels[3]->AddConstantKey(At, 1.0f);
				}
			}
			if (bPhysicalSky)
			{
				Result.Notes.Add(FString::Printf(TEXT("Sun '%s': time-of-day keyed per shot (physical sky — atmosphere colors the light)."), *SunLabel));
			}
			else
			{
				Result.Notes.Add(FString::Printf(TEXT("Sun '%s': time-of-day keyed per shot (pitch, color, intensity)."), *SunLabel));
			}
		}

		if (Sun && bAnyGodRays)
		{
			if (UDirectionalLightComponent* DirComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				DirComp->Modify();
				DirComp->bEnableLightShaftBloom = true;
				DirComp->MarkRenderStateDirty();
				Result.Notes.Add(TEXT("God rays: light-shaft bloom enabled on the sun."));
			}
		}

		// ---- Fog --------------------------------------------------------------
		if (bAnyFog || bAnyVolumetric)
		{
			AExponentialHeightFog* Fog = nullptr;
			for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
			{
				Fog = *It;
				break;
			}
			if (!Fog)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.ObjectFlags |= RF_Transactional;
				Fog = World->SpawnActor<AExponentialHeightFog>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
				if (Fog)
				{
					Fog->SetActorLabel(TEXT("CineDirector Fog"));
					Result.Notes.Add(TEXT("No height fog in the level — spawned 'CineDirector Fog'."));
				}
			}
			if (Fog)
			{
				Fog->Modify();
				UExponentialHeightFogComponent* FogComp = Fog->GetComponent();

				if (bAnyVolumetric || (bAnyGodRays && bAnyFog))
				{
					FogComp->Modify();
					FogComp->SetVolumetricFog(true);
					Result.Notes.Add(TEXT("Volumetric fog enabled."));
				}

				if (bAnyFog)
				{
					const FGuid FogActorGuid = FindOrAddPossessable(Sequence, MovieScene, Fog->GetActorLabel(), *Fog, World);
					const FGuid FogCompGuid = FindOrAddPossessable(Sequence, MovieScene, FogComp->GetName(), *FogComp, Fog, FogActorGuid);
					UMovieSceneFloatTrack* DensityTrack = FindOrAddPropertyTrack<UMovieSceneFloatTrack>(MovieScene, FogCompGuid, TEXT("FogDensity"), TEXT("FogDensity"));
					UMovieSceneFloatSection* DensitySection = GetOrCreateSection<UMovieSceneFloatSection>(DensityTrack, OverallRange);
					TArrayView<FMovieSceneFloatChannel*> DensityChannels = DensitySection->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
					if (DensityChannels.Num() > 0)
					{
						for (const TPair<const FCineShotSegment*, FFrameNumber>& Pair : SegmentStarts)
						{
							if (Pair.Key->FogDensity >= 0.0f)
							{
								DensityChannels[0]->AddConstantKey(Pair.Value, Pair.Key->FogDensity);
							}
						}
					}
					Result.Notes.Add(TEXT("Fog density keyed per shot."));
				}
			}
		}
	}

	void AddTransformKeys(
		UMovieScene3DTransformSection* Section,
		const FShotSampler& Sampler,
		FFrameNumber SegStart,
		FFrameRate TickResolution,
		FEulerContinuity& Euler,
		bool bSkipFirstKey = false,
		ULevelSequence* SequenceForFollow = nullptr)
	{
		const FCineShotSegment& Seg = Sampler.Seg;

		TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Channels.Num() < 9)
		{
			return;
		}
		Channels[6]->SetDefault(1.0);
		Channels[7]->SetDefault(1.0);
		Channels[8]->SetDefault(1.0);

		const bool bDense = Sampler.NeedsDenseKeys();
		const double Duration = FMath::Max(Seg.DurationSeconds, 0.1);
		// Follow a moving body denser so the face stays framed through the performance.
		const int32 SamplesPerSecond = Sampler.bFollowSubject
			? 12
			: (Seg.HandheldIntensity > 0.0f ? 10 : 6);
		const int32 NumKeys = bDense ? FMath::Clamp((int32)(Duration * SamplesPerSecond), 8, 400) + 1 : 2;
		// Dense samples are close enough together that linear interpolation is exact;
		// two-key shots get cubic keys whose auto tangents give the ease for free.
		const bool bLinearKeys = bDense || Seg.Easing == ECineEasing::Linear;

		const bool bScrub = SequenceForFollow != nullptr && Sampler.bFollowSubject;
		const float SavedLocalTime = bScrub ? GetSequenceLocalTimeSeconds() : 0.0f;

		for (int32 K = 0; K < NumKeys; ++K)
		{
			if (K == 0 && bSkipFirstKey)
			{
				// Continuous take: the previous segment's end key doubles as this segment's start.
				continue;
			}

			const double T = (double)K / (NumKeys - 1);
			const double TMove = (bDense && Seg.Easing == ECineEasing::EaseInOut) ? T * T * (3.0 - 2.0 * T) : T;
			const FFrameNumber Frame = SegStart + TickResolution.AsFrameTime(T * Seg.DurationSeconds).RoundToFrame();

			FVector LiveTarget = Sampler.Geo.TargetPoint;
			FVector LiveAim = Sampler.Geo.bHasLookAt ? Sampler.Geo.AimPoint : Sampler.Geo.TargetPoint;
			if (bScrub)
			{
				const double TimeSec = TickResolution.AsSeconds(Frame);
				EvaluateSequenceAtSeconds(SequenceForFollow, TimeSec);
				ResolveLiveFraming(Seg, LiveTarget, LiveAim);
			}

			FVector Pos;
			FRotator Rot;
			if (bScrub)
			{
				Sampler.Sample(TMove, T, Pos, Rot, &LiveTarget, &LiveAim);
			}
			else
			{
				Sampler.Sample(TMove, T, Pos, Rot);
			}

			// Keep euler channels continuous so a 360 orbit doesn't snap back through 0.
			if (!Euler.bValid)
			{
				Euler.Roll = Rot.Roll;
				Euler.Pitch = Rot.Pitch;
				Euler.Yaw = Rot.Yaw;
				Euler.bValid = true;
			}
			else
			{
				Euler.Roll += FMath::FindDeltaAngleDegrees(Euler.Roll, Rot.Roll);
				Euler.Pitch += FMath::FindDeltaAngleDegrees(Euler.Pitch, Rot.Pitch);
				Euler.Yaw += FMath::FindDeltaAngleDegrees(Euler.Yaw, Rot.Yaw);
			}

			const double Values[6] = { Pos.X, Pos.Y, Pos.Z, Euler.Roll, Euler.Pitch, Euler.Yaw };
			for (int32 Ch = 0; Ch < 6; ++Ch)
			{
				if (bLinearKeys)
				{
					Channels[Ch]->AddLinearKey(Frame, Values[Ch]);
				}
				else
				{
					Channels[Ch]->AddCubicKey(Frame, Values[Ch]);
				}
			}
		}

		if (bScrub)
		{
			RestoreSequenceLocalTimeSeconds(SavedLocalTime);
		}
	}

	/**
	 * Bake ManualFocusDistance keys so DOF stays on the subject as the camera
	 * moves, and so continuous takes can re-pull when the focus subject changes
	 * (engine Tracking focus can only lock one ActorToTrack per camera).
	 *
	 * FocusStart/End world points: same for normal autofocus; rack interpolates.
	 */
	void SampleFocusDistanceKeys(
		TArray<TPair<FFrameNumber, float>>& OutKeys,
		const FShotSampler& Sampler,
		FFrameNumber SegStart,
		FFrameRate TickResolution,
		const FVector& FocusStartPoint,
		const FVector& FocusEndPoint,
		bool bSkipFirstKey = false,
		ULevelSequence* SequenceForFollow = nullptr)
	{
		const FCineShotSegment& Seg = Sampler.Seg;
		const double Duration = FMath::Max(Seg.DurationSeconds, 0.1);
		// Match transform density on curved paths so focus doesn't lag the orbit.
		const bool bDense = Sampler.NeedsDenseKeys()
			|| !FocusStartPoint.Equals(FocusEndPoint, 1.0);
		const int32 SamplesPerSecond = Sampler.bFollowSubject
			? 12
			: (Seg.HandheldIntensity > 0.0f ? 10 : 6);
		const int32 NumKeys = bDense
			? FMath::Clamp((int32)(Duration * SamplesPerSecond), 8, 400) + 1
			: 2;

		const bool bScrub = SequenceForFollow != nullptr && Sampler.bFollowSubject;
		const float SavedLocalTime = bScrub ? GetSequenceLocalTimeSeconds() : 0.0f;

		for (int32 K = 0; K < NumKeys; ++K)
		{
			if (K == 0 && bSkipFirstKey)
			{
				continue;
			}

			const double T = (NumKeys <= 1) ? 0.0 : (double)K / (NumKeys - 1);
			const double TMove = (bDense && Seg.Easing == ECineEasing::EaseInOut)
				? T * T * (3.0 - 2.0 * T)
				: T;

			const FFrameNumber Frame = SegStart + TickResolution.AsFrameTime(T * Seg.DurationSeconds).RoundToFrame();
			FVector LiveTarget = Sampler.Geo.TargetPoint;
			FVector LiveAim = Sampler.Geo.bHasLookAt ? Sampler.Geo.AimPoint : Sampler.Geo.TargetPoint;
			if (bScrub)
			{
				EvaluateSequenceAtSeconds(SequenceForFollow, TickResolution.AsSeconds(Frame));
				ResolveLiveFraming(Seg, LiveTarget, LiveAim);
			}

			FVector Pos;
			FRotator Rot;
			if (bScrub)
			{
				Sampler.Sample(TMove, T, Pos, Rot, &LiveTarget, &LiveAim);
			}
			else
			{
				Sampler.Sample(TMove, T, Pos, Rot);
			}

			// When following, focus on the live face; rack still lerps bake-time endpoints.
			const FVector FocusPt = bScrub && !Seg.RackFocusToActor.IsValid()
				? LiveAim
				: FMath::Lerp(FocusStartPoint, FocusEndPoint, TMove);
			const float Dist = FMath::Max((float)FVector::Dist(Pos, FocusPt), 10.0f);
			OutKeys.Emplace(Frame, Dist);
		}

		if (bScrub)
		{
			RestoreSequenceLocalTimeSeconds(SavedLocalTime);
		}
	}

	/** Focus endpoints for a segment (rack, deep, or subject interest). */
	void ResolveFocusEndpoints(const FCineShotSegment& Seg, const FShotGeometry& Geo, FVector& OutStart, FVector& OutEnd, bool& bOutHasFocus)
	{
		bOutHasFocus = false;
		OutStart = OutEnd = Geo.AimPoint;

		if (Seg.bDeepFocus)
		{
			// Not used for world points — caller writes a large constant distance.
			bOutHasFocus = true;
			return;
		}

		const ECineShotSize Size = EffectiveShotSize(Seg);
		if (Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid())
		{
			OutStart = ActorCenter(Seg.TargetActor.Get(), Size);
			OutEnd = ActorCenter(Seg.RackFocusToActor.Get(), Size);
			bOutHasFocus = true;
			return;
		}

		if (AActor* FocusActor = ResolveFocusActor(Seg))
		{
			OutStart = OutEnd = ResolveSubjectFraming(FocusActor, Size).Point;
			bOutHasFocus = true;
			return;
		}

		if (Geo.bHasLookAt || Geo.bHasTarget)
		{
			OutStart = OutEnd = ResolveFocusPoint(Seg, Geo);
			bOutHasFocus = true;
		}
	}
}

FCineSceneContext FShotPlanExecutor::BuildSceneContext()
{
	FCineSceneContext Ctx;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || !Actor->IsListedInSceneOutliner())
			{
				continue;
			}
			// Skip level plumbing and our own output so "the camera" never matches a spawned shot.
			if (Actor->IsA<AWorldSettings>() || Actor->IsA<ABrush>() || Actor->IsA<ACineCameraActor>())
			{
				continue;
			}

			FCineSceneActorInfo Info;
			Info.Actor = Actor;
			Info.Label = Actor->GetActorLabel();
			const FBox Bounds = Actor->GetComponentsBoundingBox(true);
			Info.Location = Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
			Info.BoundsRadius = Bounds.IsValid ? FMath::Max<double>(Bounds.GetExtent().Size(), 25.0) : 100.0;
			Info.Facing = Actor->GetActorRotation();
			Ctx.Actors.Add(MoveTemp(Info));
		}
	}

	if (GCurrentLevelEditingViewportClient)
	{
		Ctx.ViewportLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
		Ctx.ViewportRotation = GCurrentLevelEditingViewportClient->GetViewRotation();
	}

	return Ctx;
}

namespace CineDirectorExec
{
	bool IsCineDirectorCameraName(const FString& Name)
	{
		return Name.StartsWith(TEXT("CineDirector Shot"), ESearchCase::IgnoreCase)
			|| Name.StartsWith(TEXT("CineDirector Take"), ESearchCase::IgnoreCase);
	}

	bool IsCineDirectorCameraLabel(const FString& Label)
	{
		return IsCineDirectorCameraName(Label);
	}

	void CollectPossessableTree(UMovieScene* MovieScene, const FGuid& Root, TArray<FGuid>& Out)
	{
		Out.AddUnique(Root);
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
			if (P.GetParent() == Root)
			{
				CollectPossessableTree(MovieScene, P.GetGuid(), Out);
			}
		}
	}

	/**
	 * Tear down previous CineDirector cameras + their cut sections so Create Shots
	 * always matches the current prompt (multi-cut and continuous).
	 */
	void RemovePriorCineDirectorShots(
		ULevelSequence* Sequence,
		UMovieScene* MovieScene,
		UWorld* World,
		int32& OutRemovedCams,
		int32& OutRemovedCuts)
	{
		OutRemovedCams = 0;
		OutRemovedCuts = 0;
		if (!Sequence || !MovieScene || !World)
		{
			return;
		}

		TArray<FGuid> Roots;
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
			if (IsCineDirectorCameraName(P.GetName()))
			{
				Roots.Add(P.GetGuid());
			}
		}

		TSet<FGuid> AllGuids;
		for (const FGuid& Root : Roots)
		{
			TArray<FGuid> Tree;
			CollectPossessableTree(MovieScene, Root, Tree);
			for (const FGuid& G : Tree)
			{
				AllGuids.Add(G);
			}
		}

		// Drop camera-cut sections that point at those cameras.
		if (UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack()))
		{
			CutTrack->Modify();
			TArray<UMovieSceneSection*> CutSections = CutTrack->GetAllSections();
			for (UMovieSceneSection* Section : CutSections)
			{
				UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
				if (!Cut)
				{
					continue;
				}
				const FGuid BoundGuid = Cut->GetCameraBindingID().GetGuid();
				if (AllGuids.Contains(BoundGuid))
				{
					CutTrack->RemoveSection(*Cut);
					++OutRemovedCuts;
				}
			}
		}

		// Destroy bound world actors (cameras), then strip possessables (children first).
		// Children before parents so RemovePossessable doesn't leave orphans.
		TArray<FGuid> Ordered = AllGuids.Array();
		Ordered.Sort([&](const FGuid& A, const FGuid& B)
		{
			const FMovieScenePossessable* PA = MovieScene->FindPossessable(A);
			const FMovieScenePossessable* PB = MovieScene->FindPossessable(B);
			const bool bAChild = PA && AllGuids.Contains(PA->GetParent());
			const bool bBChild = PB && AllGuids.Contains(PB->GetParent());
			if (bAChild != bBChild)
			{
				return bAChild && !bBChild;
			}
			return GetTypeHash(A) < GetTypeHash(B);
		});

		TSet<AActor*> DestroyedActors;
		for (const FGuid& Guid : Ordered)
		{
			TArray<UObject*, TInlineAllocator<1>> BoundObjects;
			Sequence->LocateBoundObjects(Guid, World, BoundObjects);
			for (UObject* Obj : BoundObjects)
			{
				if (AActor* Actor = Cast<AActor>(Obj))
				{
					if (!DestroyedActors.Contains(Actor))
					{
						DestroyedActors.Add(Actor);
						World->DestroyActor(Actor);
						++OutRemovedCams;
					}
				}
			}

			Sequence->UnbindPossessableObjects(Guid);
			MovieScene->RemovePossessable(Guid);
		}

		// Orphan CineDirector cameras left in the level without a binding.
		for (TActorIterator<ACineCameraActor> It(World); It; ++It)
		{
			ACineCameraActor* Cam = *It;
			if (!Cam || !IsCineDirectorCameraLabel(Cam->GetActorLabel()) || DestroyedActors.Contains(Cam))
			{
				continue;
			}
			World->DestroyActor(Cam);
			++OutRemovedCams;
		}
	}
} // namespace CineDirectorExec (helpers used by Execute)

FCineExecuteResult FShotPlanExecutor::Execute(const FCineShotPlan& Plan)
{
	using namespace CineDirectorExec;

	FCineExecuteResult Result;

	if (Plan.Segments.Num() == 0)
	{
		Result.Error = LOCTEXT("EmptyPlan", "The shot plan contains no shots.");
		return Result;
	}

	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!Sequence || !Sequence->GetMovieScene())
	{
		Result.Error = LOCTEXT("NoSequence", "No Level Sequence is open in Sequencer. Open one first (or create one via the Cinematics toolbar button).");
		return Result;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Result.Error = LOCTEXT("NoWorld", "No editor world available.");
		return Result;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();

	FVector ViewLoc = FVector::ZeroVector;
	FRotator ViewRot = FRotator::ZeroRotator;
	if (GCurrentLevelEditingViewportClient)
	{
		ViewLoc = GCurrentLevelEditingViewportClient->GetViewLocation();
		ViewRot = GCurrentLevelEditingViewportClient->GetViewRotation();
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateShotsTransaction", "CineDirector: Create Shots"));
	Sequence->Modify();
	MovieScene->Modify();

	// Replace prior CineDirector work so re-running always matches the latest prompt
	// (multi-cut used to append after old continuous takes and look "stuck").
	int32 RemovedCams = 0;
	int32 RemovedCuts = 0;
	RemovePriorCineDirectorShots(Sequence, MovieScene, World, RemovedCams, RemovedCuts);

	// Always author from the start of the playback range (not after old cuts).
	FFrameNumber Cursor = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
	if (Plan.bCreateCameraCuts && !CutTrack)
	{
		CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}
	if (CutTrack)
	{
		CutTrack->Modify();
	}

	if (RemovedCams > 0 || RemovedCuts > 0)
	{
		Result.Notes.Add(FString::Printf(
			TEXT("Replaced prior CineDirector work (%d camera(s), %d cut section(s))."),
			RemovedCams, RemovedCuts));
	}

	if (Plan.bOneContinuousShot)
	{
		// ---- One camera, every move chained into a single unbroken take -------
		const FCineShotSegment& First = Plan.Segments[0];
		// Frame body performance at the take start so CU geometry hits the live face.
		const float SavedScrubTime = GetSequenceLocalTimeSeconds();
		EvaluateSequenceAtSeconds(Sequence, TickResolution.AsSeconds(Cursor));
		if (AActor* T = First.TargetActor.Get()) { RefreshActorBones(T); }
		if (AActor* L = First.LookAtActor.Get()) { RefreshActorBones(L); }

		const FShotGeometry FirstGeo = ComputeGeometry(First, ViewLoc, ViewRot);
		const FShotSampler FirstSampler(First, FirstGeo, ViewRot);

		FVector StartPos;
		FRotator StartRot;
		FirstSampler.Sample(0.0, 0.0, StartPos, StartRot);
		RestoreSequenceLocalTimeSeconds(SavedScrubTime);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(StartPos, StartRot, SpawnParams);
		if (!Camera)
		{
			Result.Error = LOCTEXT("TakeSpawnFailed", "Failed to spawn the take's camera.");
			return Result;
		}

		const int32 TakeNumber = (CutTrack ? CutTrack->GetAllSections().Num() : 0) + 1;
		const FString Label = First.CameraLabel.IsEmpty()
			? FString::Printf(TEXT("CineDirector Take %d"), TakeNumber)
			: First.CameraLabel;
		Camera->SetActorLabel(Label);

		UCineCameraComponent* Lens = Camera->GetCineCameraComponent();
		float CurrentFocal = First.FocalLengthMm > 0.0f ? First.FocalLengthMm : 35.0f;
		Lens->CurrentFocalLength = CurrentFocal;
		Lens->CurrentAperture = First.Aperture > 0.0f ? First.Aperture : 2.8f;

		const FGuid CamGuid = MovieScene->AddPossessable(Label, Camera->GetClass());
		Sequence->BindPossessableObject(CamGuid, *Camera, World);

		UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CamGuid);
		UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		TransformTrack->AddSection(*TransformSection);

		const FFrameNumber TakeStart = Cursor;
		FEulerContinuity Euler;

		FCineShotSegment MergedLook = First;
		TArray<TPair<FFrameNumber, float>> FocalKeys;
		TArray<TPair<FFrameNumber, float>> FocusKeys;
		TArray<TPair<const FCineShotSegment*, FFrameNumber>> SegmentStarts;

		// Continuous take can only hold one Tracking ActorToTrack on the lens.
		// Single shared subject → engine Tracking (stays sharp as the camera moves,
		// and if the actor animates). Subject changes / racks / fixed / deep → bake
		// ManualFocusDistance keys so each move re-pulls focus.
		AActor* SharedFocusActor = nullptr;
		bool bFocusSubjectChanges = false;
		bool bAnyRackFocus = false;
		bool bAnyDeepFocus = false;
		bool bAnyFixedFocus = false;
		bool bAnyAutofocus = false;
		for (const FCineShotSegment& S : Plan.Segments)
		{
			if (S.RackFocusToActor.IsValid() && S.TargetActor.IsValid())
			{
				bAnyRackFocus = true;
			}
			if (S.bDeepFocus)
			{
				bAnyDeepFocus = true;
			}
			if (S.bFixedFocus)
			{
				bAnyFixedFocus = true;
			}
			if (S.bTrackFocus || ResolveFocusActor(S))
			{
				bAnyAutofocus = true;
			}
			if (AActor* FocusA = ResolveFocusActor(S))
			{
				if (!SharedFocusActor)
				{
					SharedFocusActor = FocusA;
				}
				else if (FocusA != SharedFocusActor)
				{
					bFocusSubjectChanges = true;
				}
			}
		}
		const bool bUseTrackingFocus =
			SharedFocusActor != nullptr
			&& !bAnyRackFocus
			&& !bFocusSubjectChanges
			&& !bAnyDeepFocus
			&& !bAnyFixedFocus
			&& bAnyAutofocus;
		const bool bBakeFocusKeys = !bUseTrackingFocus && (bAnyAutofocus || bAnyRackFocus || bAnyDeepFocus || bAnyFixedFocus || SharedFocusActor != nullptr);

		if (bUseTrackingFocus)
		{
			ApplyTrackingFocus(Lens, SharedFocusActor, EffectiveShotSize(First));
		}

		FVector PrevPos = StartPos;
		FRotator PrevRot = StartRot;

		int32 MoveIndex = 1;
		for (const FCineShotSegment& SourceSeg : Plan.Segments)
		{
			const bool bFirst = (MoveIndex == 1);
			FCineShotSegment Seg = SourceSeg;
			const FShotGeometry Geo = bFirst ? FirstGeo : ComputeGeometryChained(Seg, PrevPos, PrevRot);

			// Mid-take framing becomes an implicit dolly: "close up on X" travels to
			// close-up (face) distance instead of holding still where the last move ended.
			{
				const ECineShotSize Size = EffectiveShotSize(Seg);
				if (!bFirst && Seg.Move == ECineMoveType::Static && Size != ECineShotSize::Unspecified && Geo.bHasTarget)
				{
					const double LensScale = Seg.FocalLengthMm > 0.0f ? Seg.FocalLengthMm / 35.0 : 1.0;
					const double WantDist = FMath::Max(Geo.Radius * FramingFactor(Size) * LensScale, 40.0);
					const double Delta = Geo.Distance - WantDist;
					if (FMath::Abs(Delta) > 25.0)
					{
						Seg.Move = Delta > 0.0 ? ECineMoveType::DollyIn : ECineMoveType::DollyOut;
						Seg.MoveAmount = FMath::Abs(Delta);
						Seg.ParseNotes.Remove(TEXT("No camera move recognized — using a static shot."));
					}
				}
			}

			const FShotSampler Sampler(Seg, Geo, bFirst ? ViewRot : PrevRot, /*bInChained*/ !bFirst);

			FFrameNumber DurFrames = TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame();
			DurFrames = FMath::Max(DurFrames, FFrameNumber(1));
			const FFrameNumber SegStart = Cursor;
			const FFrameNumber SegEnd = SegStart + DurFrames;

			FVector SegStartPos, SegEndPos;
			FRotator SegStartRot, SegEndRot;
			{
				// End pose uses live subject at SegEnd so the next chained move continues from the right place.
				const float SavedT = GetSequenceLocalTimeSeconds();
				FVector LiveT = Geo.TargetPoint, LiveA = Geo.AimPoint;
				EvaluateSequenceAtSeconds(Sequence, TickResolution.AsSeconds(SegStart));
				if (Sampler.bFollowSubject) { ResolveLiveFraming(Seg, LiveT, LiveA); }
				Sampler.Sample(0.0, 0.0, SegStartPos, SegStartRot, Sampler.bFollowSubject ? &LiveT : nullptr, Sampler.bFollowSubject ? &LiveA : nullptr);
				EvaluateSequenceAtSeconds(Sequence, TickResolution.AsSeconds(SegEnd));
				if (Sampler.bFollowSubject) { ResolveLiveFraming(Seg, LiveT, LiveA); }
				Sampler.Sample(1.0, 1.0, SegEndPos, SegEndRot, Sampler.bFollowSubject ? &LiveT : nullptr, Sampler.bFollowSubject ? &LiveA : nullptr);
				RestoreSequenceLocalTimeSeconds(SavedT);
			}

			// Always pass Sequence so continuous moves re-sample the body at every key.
			AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler, /*bSkipFirstKey*/ !bFirst, Sequence);
			// Point at the plan segment (stable), not the stack copy `Seg`.
			SegmentStarts.Emplace(&SourceSeg, SegStart);

			// Zooms and mid-take lens changes become keys on one shared focal-length track.
			if (Seg.Move == ECineMoveType::ZoomIn || Seg.Move == ECineMoveType::ZoomOut)
			{
				float EndFocal = Seg.MoveAmount > 0.0
					? (float)Seg.MoveAmount
					: (Seg.Move == ECineMoveType::ZoomIn ? CurrentFocal * 2.0f : CurrentFocal * 0.5f);
				EndFocal = FMath::Clamp(EndFocal, 4.0f, 1000.0f);
				FocalKeys.Emplace(SegStart, CurrentFocal);
				FocalKeys.Emplace(SegEnd, EndFocal);
				CurrentFocal = EndFocal;
			}
			else if (!bFirst && Seg.FocalLengthMm > 0.0f && !FMath::IsNearlyEqual(Seg.FocalLengthMm, CurrentFocal))
			{
				FocalKeys.Emplace(SegStart, CurrentFocal);
				FocalKeys.Emplace(SegEnd, Seg.FocalLengthMm);
				CurrentFocal = Seg.FocalLengthMm;
			}

			// Focus pull keys for multi-subject / rack / fixed continuous takes.
			if (bBakeFocusKeys)
			{
				if (Seg.bDeepFocus)
				{
					if (!bFirst || FocusKeys.Num() == 0)
					{
						FocusKeys.Emplace(SegStart, 100000.0f);
					}
					FocusKeys.Emplace(SegEnd, 100000.0f);
				}
				else if (Seg.bFixedFocus)
				{
					// Hold the distance from the start of this move (no re-pull).
					FVector FocusPt = ResolveFocusPoint(Seg, Geo);
					const float FixedDist = FMath::Max((float)FVector::Dist(SegStartPos, FocusPt), 10.0f);
					if (!bFirst || FocusKeys.Num() == 0)
					{
						FocusKeys.Emplace(SegStart, FixedDist);
					}
					FocusKeys.Emplace(SegEnd, FixedDist);
				}
				else
				{
					FVector FocusStartPt, FocusEndPt;
					bool bHasFocus = false;
					ResolveFocusEndpoints(Seg, Geo, FocusStartPt, FocusEndPt, bHasFocus);
					if (bHasFocus)
					{
						SampleFocusDistanceKeys(
							FocusKeys, Sampler, SegStart, TickResolution,
							FocusStartPt, FocusEndPt, /*bSkipFirstKey*/ !bFirst && FocusKeys.Num() > 0, Sequence);
					}
				}
			}

			MergeLookInto(MergedLook, Seg);

			FString NoteLine = FString::Printf(TEXT("Move %d:"), MoveIndex);
			if (!Seg.TargetLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(" on '%s'"), *Seg.TargetLabel);
			}
			if (!Seg.LookAtLabel.IsEmpty() && Seg.LookAtLabel != Seg.TargetLabel)
			{
				NoteLine += FString::Printf(TEXT(", looking at '%s'"), *Seg.LookAtLabel);
			}
			if (!Seg.RackFocusToLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
			}
			else if (Seg.bDeepFocus)
			{
				NoteLine += TEXT(", deep focus");
			}
			else if (Seg.bFixedFocus)
			{
				NoteLine += TEXT(", fixed focus");
			}
			else if (bUseTrackingFocus || (bBakeFocusKeys && ResolveFocusActor(Seg)))
			{
				NoteLine += TEXT(", autofocus");
			}
			if (Sampler.bFollowSubject)
			{
				NoteLine += TEXT(", subject follow");
			}
			if (!Seg.StyleKitName.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(", style '%s'"), *Seg.StyleKitName);
			}
			NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
			Result.Notes.Add(MoveTemp(NoteLine));
			for (const FString& Note : Seg.ParseNotes)
			{
				Result.Notes.Add(FString::Printf(TEXT("Move %d: %s"), MoveIndex, *Note));
			}

			UE_LOG(LogCineDirector, Log, TEXT("Take move %d: clause \"%s\", target '%s', follow=%s, %.1fs, frames %d-%d"),
				MoveIndex, *Seg.RawText,
				Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
				Sampler.bFollowSubject ? TEXT("yes") : TEXT("no"),
				Seg.DurationSeconds, SegStart.Value, SegEnd.Value);

			Result.TotalDurationSeconds += Seg.DurationSeconds;
			PrevPos = SegEndPos;
			PrevRot = SegEndRot;
			Cursor = SegEnd;
			++MoveIndex;
		}

		TransformSection->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));

		if (bBakeFocusKeys && FocusKeys.Num() > 0)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.bSmoothFocusChanges = false;
			// Seed the component value to the first key so the first frame is sharp
			// even before the section evaluates.
			Lens->FocusSettings.ManualFocusDistance = FocusKeys[0].Value;
		}
		else if (!bUseTrackingFocus && FirstGeo.bHasTarget)
		{
			ApplyFixedFocus(Lens, (float)FVector::Dist(StartPos, FirstGeo.AimPoint));
		}

		ApplyCameraLook(Lens, MergedLook);

		FGuid LensGuid;
		auto GetLensBinding = [&]() -> FGuid
		{
			if (!LensGuid.IsValid())
			{
				LensGuid = MovieScene->AddPossessable(Lens->GetName(), Lens->GetClass());
				if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(LensGuid))
				{
					Possessable->SetParent(CamGuid, MovieScene);
				}
				Sequence->BindPossessableObject(LensGuid, *Lens, Camera);
			}
			return LensGuid;
		};

		auto AddLensKeysTrack = [&](FName PropertyName, const FString& PropertyPath, const TArray<TPair<FFrameNumber, float>>& Keys)
		{
			UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(GetLensBinding());
			Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
			UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));
			Track->AddSection(*Section);
			TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			if (Channels.Num() > 0)
			{
				for (const TPair<FFrameNumber, float>& Key : Keys)
				{
					Channels[0]->AddCubicKey(Key.Key, Key.Value);
				}
			}
		};

		if (FocalKeys.Num() > 0)
		{
			AddLensKeysTrack(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"), FocalKeys);
		}
		if (bBakeFocusKeys && FocusKeys.Num() > 0)
		{
			AddLensKeysTrack(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"), FocusKeys);
		}

		if (Plan.bCreateCameraCuts && CutTrack)
		{
			if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(CamGuid), TakeStart))
			{
				CutSection->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));
			}
		}

		ApplyLightingTracks(Sequence, MovieScene, World, SegmentStarts, TRange<FFrameNumber>(TakeStart, Cursor), Result);

		FString TakeFocusNote;
		if (bUseTrackingFocus && SharedFocusActor)
		{
			TakeFocusNote = FString::Printf(TEXT(", tracking focus on '%s'"), *SharedFocusActor->GetActorLabel());
		}
		else if (bBakeFocusKeys)
		{
			TakeFocusNote = bFocusSubjectChanges || bAnyRackFocus
				? TEXT(", focus pulls keyed per move")
				: TEXT(", focus distance keyed to camera path");
		}
		const bool bAnyFollow = Plan.Segments.ContainsByPredicate(
			[](const FCineShotSegment& S) { return S.TargetActor.IsValid(); });
		Result.Notes.Insert(FString::Printf(
			TEXT("Continuous take: camera '%s', %d moves%s%s"),
			*Label, Plan.Segments.Num(), *TakeFocusNote,
			bAnyFollow ? TEXT(", subject follow baked (tracks body motion)") : TEXT("")), 0);
		Result.NumShots = 1;

		const TRange<FFrameNumber> TakePlayback = MovieScene->GetPlaybackRange();
		if (Cursor > TakePlayback.GetUpperBoundValue())
		{
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(TakePlayback.GetLowerBoundValue(), Cursor));
		}

		MovieScene->MarkPackageDirty();
		ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();

		Result.bSuccess = true;
		UE_LOG(LogCineDirector, Log, TEXT("Created continuous take '%s': %d moves, %.1fs total."),
			*Label, Plan.Segments.Num(), Result.TotalDurationSeconds);
		return Result;
	}

	const FFrameNumber PlanStart = Cursor;
	TArray<TPair<const FCineShotSegment*, FFrameNumber>> SegmentStarts;

	int32 ShotIndex = 1;
	for (const FCineShotSegment& Seg : Plan.Segments)
	{
		// Body performance may have moved the subject — frame geometry at this cut's start.
		const float SavedScrubTime = GetSequenceLocalTimeSeconds();
		EvaluateSequenceAtSeconds(Sequence, TickResolution.AsSeconds(Cursor));
		if (AActor* T = Seg.TargetActor.Get()) { RefreshActorBones(T); }
		if (AActor* L = Seg.LookAtActor.Get()) { RefreshActorBones(L); }

		const FShotGeometry Geo = ComputeGeometry(Seg, ViewLoc, ViewRot);
		const FShotSampler Sampler(Seg, Geo, ViewRot);

		FVector StartPos, EndPos;
		FRotator StartRot, EndRot;
		FVector LiveT = Geo.TargetPoint, LiveA = Geo.AimPoint;
		if (Sampler.bFollowSubject) { ResolveLiveFraming(Seg, LiveT, LiveA); }
		Sampler.Sample(0.0, 0.0, StartPos, StartRot, Sampler.bFollowSubject ? &LiveT : nullptr, Sampler.bFollowSubject ? &LiveA : nullptr);
		const FFrameNumber TentativeEnd = Cursor + FMath::Max(TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame(), FFrameNumber(1));
		EvaluateSequenceAtSeconds(Sequence, TickResolution.AsSeconds(TentativeEnd));
		if (Sampler.bFollowSubject) { ResolveLiveFraming(Seg, LiveT, LiveA); }
		Sampler.Sample(1.0, 1.0, EndPos, EndRot, Sampler.bFollowSubject ? &LiveT : nullptr, Sampler.bFollowSubject ? &LiveA : nullptr);
		RestoreSequenceLocalTimeSeconds(SavedScrubTime);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(StartPos, StartRot, SpawnParams);
		if (!Camera)
		{
			Result.Notes.Add(FString::Printf(TEXT("Shot %d: failed to spawn a camera — skipped."), ShotIndex));
			++ShotIndex;
			continue;
		}

		const FString Label = Seg.CameraLabel.IsEmpty()
			? FString::Printf(TEXT("CineDirector Shot %d"), ShotIndex)
			: Seg.CameraLabel;
		Camera->SetActorLabel(Label);

		UCineCameraComponent* Lens = Camera->GetCineCameraComponent();
		Lens->CurrentFocalLength = Seg.FocalLengthMm > 0.0f ? Seg.FocalLengthMm : 35.0f;
		Lens->CurrentAperture = Seg.Aperture > 0.0f ? Seg.Aperture : 2.8f;
		ApplyCameraLook(Lens, Seg);

		const bool bRackFocus = Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid();
		const bool bBakeShotFocus =
			bRackFocus
			|| Seg.bFixedFocus
			|| Seg.bDeepFocus
			// Moving camera + subject: bake distance so DOF re-pulls even if Tracking
			// isn't evaluating (common when the cut lands mid-move).
			|| ((Seg.bTrackFocus || ResolveFocusActor(Seg) != nullptr)
				&& Seg.Move != ECineMoveType::Static
				&& Seg.Move != ECineMoveType::ZoomIn
				&& Seg.Move != ECineMoveType::ZoomOut
				&& Seg.Move != ECineMoveType::PanLeft
				&& Seg.Move != ECineMoveType::PanRight
				&& Seg.Move != ECineMoveType::TiltUp
				&& Seg.Move != ECineMoveType::TiltDown);

		if (Seg.bDeepFocus)
		{
			ApplyDeepFocus(Lens);
		}
		else if (Seg.bFixedFocus)
		{
			ApplyFixedFocus(Lens, (float)FVector::Dist(StartPos, ResolveFocusPoint(Seg, Geo)));
		}
		else if (bRackFocus)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.bSmoothFocusChanges = true;
			Lens->FocusSettings.FocusSmoothingInterpSpeed = 6.0f;
			const ECineShotSize Size = EffectiveShotSize(Seg);
			Lens->FocusSettings.ManualFocusDistance =
				(float)FVector::Dist(StartPos, ActorCenter(Seg.TargetActor.Get(), Size));
		}
		else if (AActor* FocusActor = ResolveFocusActor(Seg))
		{
			// Tracking keeps DOF on the actor (and follows if they animate). For
			// dollies/orbits we also bake Manual keys below so Sequencer always
			// has an explicit distance curve — but Tracking is the live method
			// when we are not baking.
			if (bBakeShotFocus)
			{
				Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
				Lens->FocusSettings.bSmoothFocusChanges = false;
				Lens->FocusSettings.ManualFocusDistance =
					(float)FVector::Dist(StartPos, ResolveFocusPoint(Seg, Geo));
			}
			else
			{
				ApplyTrackingFocus(Lens, FocusActor, EffectiveShotSize(Seg));
			}
		}
		else if (Geo.bHasTarget || Geo.bHasLookAt)
		{
			ApplyFixedFocus(Lens, (float)FVector::Dist(StartPos, Geo.AimPoint));
		}

		const FGuid CamGuid = MovieScene->AddPossessable(Label, Camera->GetClass());
		Sequence->BindPossessableObject(CamGuid, *Camera, World);

		FFrameNumber DurFrames = TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame();
		DurFrames = FMath::Max(DurFrames, FFrameNumber(1));
		const FFrameNumber SegStart = Cursor;
		const FFrameNumber SegEnd = SegStart + DurFrames;

		UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CamGuid);
		UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		TransformSection->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
		TransformTrack->AddSection(*TransformSection);
		FEulerContinuity Euler;
		AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler, /*bSkipFirstKey*/ false, Sequence);

		// The lens component only needs its own binding when a lens property is animated.
		FGuid LensGuid;
		auto GetLensBinding = [&]() -> FGuid
		{
			if (!LensGuid.IsValid())
			{
				LensGuid = MovieScene->AddPossessable(Lens->GetName(), Lens->GetClass());
				if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(LensGuid))
				{
					Possessable->SetParent(CamGuid, MovieScene);
				}
				Sequence->BindPossessableObject(LensGuid, *Lens, Camera);
			}
			return LensGuid;
		};

		auto AddLensFloatTrack = [&](FName PropertyName, const FString& PropertyPath, float StartValue, float EndValue)
		{
			UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(GetLensBinding());
			Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
			UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
			Track->AddSection(*Section);

			TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			if (Channels.Num() > 0)
			{
				if (Seg.Easing == ECineEasing::Linear)
				{
					Channels[0]->AddLinearKey(SegStart, StartValue);
					Channels[0]->AddLinearKey(SegEnd, EndValue);
				}
				else
				{
					Channels[0]->AddCubicKey(SegStart, StartValue);
					Channels[0]->AddCubicKey(SegEnd, EndValue);
				}
			}
		};

		if (Seg.Move == ECineMoveType::ZoomIn || Seg.Move == ECineMoveType::ZoomOut)
		{
			const float StartFocal = Lens->CurrentFocalLength;
			float EndFocal = Seg.MoveAmount > 0.0
				? (float)Seg.MoveAmount
				: (Seg.Move == ECineMoveType::ZoomIn ? StartFocal * 2.0f : StartFocal * 0.5f);
			EndFocal = FMath::Clamp(EndFocal, 4.0f, 1000.0f);
			AddLensFloatTrack(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"), StartFocal, EndFocal);
		}

		if (bBakeShotFocus && !Seg.bDeepFocus)
		{
			FVector FocusStartPt, FocusEndPt;
			bool bHasFocus = false;
			ResolveFocusEndpoints(Seg, Geo, FocusStartPt, FocusEndPt, bHasFocus);
			if (bHasFocus || Seg.bFixedFocus)
			{
				TArray<TPair<FFrameNumber, float>> FocusKeys;
				if (Seg.bFixedFocus)
				{
					const float FixedDist = FMath::Max((float)FVector::Dist(StartPos, ResolveFocusPoint(Seg, Geo)), 10.0f);
					FocusKeys.Emplace(SegStart, FixedDist);
					FocusKeys.Emplace(SegEnd, FixedDist);
				}
				else
				{
					SampleFocusDistanceKeys(FocusKeys, Sampler, SegStart, TickResolution, FocusStartPt, FocusEndPt, false, Sequence);
				}

				if (FocusKeys.Num() >= 2)
				{
					// Dense path: write the full key curve (not just start/end).
					UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(GetLensBinding());
					Track->SetPropertyNameAndPath(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"));
					UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
					Section->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
					Track->AddSection(*Section);
					TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
					if (Channels.Num() > 0)
					{
						const bool bLinearKeys = Sampler.NeedsDenseKeys() || Seg.Easing == ECineEasing::Linear || bRackFocus;
						for (const TPair<FFrameNumber, float>& Key : FocusKeys)
						{
							if (bLinearKeys)
							{
								Channels[0]->AddLinearKey(Key.Key, Key.Value);
							}
							else
							{
								Channels[0]->AddCubicKey(Key.Key, Key.Value);
							}
						}
					}
					if (FocusKeys.Num() > 0)
					{
						Lens->FocusSettings.ManualFocusDistance = FocusKeys[0].Value;
					}
				}
			}
		}
		else if (Seg.bDeepFocus)
		{
			// Hold deep plane for the whole cut.
			AddLensFloatTrack(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"), 100000.0f, 100000.0f);
		}

		if (Plan.bCreateCameraCuts && CutTrack)
		{
			if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(CamGuid), SegStart))
			{
				CutSection->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
			}
		}

		FString NoteLine = FString::Printf(TEXT("Shot %d: '%s'"), ShotIndex, *Label);
		if (!Seg.TargetLabel.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(" on '%s'"), *Seg.TargetLabel);
		}
		if (!Seg.LookAtLabel.IsEmpty() && Seg.LookAtLabel != Seg.TargetLabel)
		{
			NoteLine += FString::Printf(TEXT(", looking at '%s'"), *Seg.LookAtLabel);
		}
		if (!Seg.RackFocusToLabel.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
		}
		else if (Seg.bDeepFocus)
		{
			NoteLine += TEXT(", deep focus");
		}
		else if (Seg.bFixedFocus)
		{
			NoteLine += TEXT(", fixed focus");
		}
		else if (ResolveFocusActor(Seg))
		{
			NoteLine += bBakeShotFocus ? TEXT(", autofocus (path)") : TEXT(", tracking focus");
		}
		if (Sampler.bFollowSubject)
		{
			NoteLine += TEXT(", subject follow");
		}
		if (!Seg.StyleKitName.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(", style '%s'"), *Seg.StyleKitName);
		}
		NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
		Result.Notes.Add(MoveTemp(NoteLine));
		for (const FString& Note : Seg.ParseNotes)
		{
			Result.Notes.Add(FString::Printf(TEXT("Shot %d: %s"), ShotIndex, *Note));
		}

		UE_LOG(LogCineDirector, Log, TEXT("Shot %d: camera '%s', target '%s', follow=%s, clause \"%s\", %.1fs, frames %d-%d"),
			ShotIndex, *Label,
			Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
			Sampler.bFollowSubject ? TEXT("yes") : TEXT("no"),
			*Seg.RawText, Seg.DurationSeconds, SegStart.Value, SegEnd.Value);
		for (const FString& Note : Seg.ParseNotes)
		{
			UE_LOG(LogCineDirector, Warning, TEXT("Shot %d: %s"), ShotIndex, *Note);
		}

		Result.TotalDurationSeconds += Seg.DurationSeconds;
		++Result.NumShots;
		SegmentStarts.Emplace(&Seg, SegStart);
		Cursor = SegEnd;
		++ShotIndex;
	}

	if (Result.NumShots == 0)
	{
		Result.Error = LOCTEXT("NothingCreated", "Couldn't create any cameras from the plan.");
		return Result;
	}

	ApplyLightingTracks(Sequence, MovieScene, World, SegmentStarts, TRange<FFrameNumber>(PlanStart, Cursor), Result);

	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	if (Cursor > Playback.GetUpperBoundValue())
	{
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Playback.GetLowerBoundValue(), Cursor));
	}

	MovieScene->MarkPackageDirty();
	ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();

	const bool bAnyFollow = Plan.Segments.ContainsByPredicate(
		[](const FCineShotSegment& S) { return S.TargetActor.IsValid() || S.bFollowSubjectPosition; });
	Result.Notes.Insert(FString::Printf(
		TEXT("Multi-cut: %d shots, %.1fs total%s"),
		Result.NumShots, Result.TotalDurationSeconds,
		bAnyFollow ? TEXT(", subject follow baked (tracks body motion)") : TEXT("")), 0);

	Result.bSuccess = true;
	UE_LOG(LogCineDirector, Log, TEXT("Created %d multi-cut shot(s), %.1fs total, follow=%s, in '%s'."),
		Result.NumShots, Result.TotalDurationSeconds,
		bAnyFollow ? TEXT("yes") : TEXT("no"),
		*Sequence->GetName());
	return Result;
}

#undef LOCTEXT_NAMESPACE
