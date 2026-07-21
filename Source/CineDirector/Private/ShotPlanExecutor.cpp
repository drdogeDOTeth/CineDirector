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
	 * Best head/face bone world location on a skeletal actor (VRM/MMD/Mixamo/UE).
	 * Returns false for props / meshes without a recognizable head.
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

		TArray<USkeletalMeshComponent*> Meshes;
		Actor->GetComponents<USkeletalMeshComponent>(Meshes);
		for (USkeletalMeshComponent* Skel : Meshes)
		{
			if (!Skel || !Skel->GetSkeletalMeshAsset())
			{
				continue;
			}

			for (const FName& Bone : PreferredBones)
			{
				if (Skel->GetBoneIndex(Bone) != INDEX_NONE)
				{
					OutHead = Skel->GetBoneLocation(Bone);
					// Head bone is often at the jaw/base — nudge toward eyes.
					OutHead.Z += 6.0;
					return true;
				}
			}

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
				// Prefer short exact-ish names ("head") over long chains.
				const int32 Score = 100 - Name.Len() + (Name == TEXT("head") ? 50 : 0);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestIdx = i;
				}
			}
			if (BestIdx != INDEX_NONE)
			{
				OutHead = Skel->GetBoneLocation(RefSkel.GetBoneName(BestIdx));
				OutHead.Z += 6.0;
				return true;
			}
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

		switch (Size)
		{
		case ECineShotSize::ExtremeCloseUp:
			Out.Point = bHasHead ? HeadLoc : BoundsHeightPoint(Actor, 0.93);
			// Face-scale radius so CU distance isn't "full body * 1.6".
			Out.Radius = bHasHead
				? FMath::Clamp(BodyRadius * 0.14, 10.0, 28.0)
				: BodyRadius * 0.18;
			break;

		case ECineShotSize::CloseUp:
			Out.Point = bHasHead ? HeadLoc : BoundsHeightPoint(Actor, 0.90);
			Out.Radius = bHasHead
				? FMath::Clamp(BodyRadius * 0.18, 12.0, 36.0)
				: BodyRadius * 0.22;
			break;

		case ECineShotSize::MediumCloseUp:
			// Chest-up: head, slightly lower so shoulders read.
			if (bHasHead)
			{
				Out.Point = HeadLoc;
				Out.Point.Z -= FMath::Clamp(BodyRadius * 0.06, 8.0, 22.0);
			}
			else
			{
				Out.Point = BoundsHeightPoint(Actor, 0.80);
			}
			Out.Radius = BodyRadius * 0.38;
			break;

		case ECineShotSize::Medium:
			Out.Point = bHasHead
				? FMath::Lerp(BodyCenter, HeadLoc, 0.55)
				: BoundsHeightPoint(Actor, 0.62);
			Out.Radius = BodyRadius * 0.72;
			break;

		case ECineShotSize::Unspecified:
			// Default "from front" on a character: upper body / face, not pelvis.
			if (bHasHead)
			{
				Out.Point = FMath::Lerp(BodyCenter, HeadLoc, 0.70);
				Out.Radius = BodyRadius * 0.85;
			}
			else
			{
				Out.Point = BodyCenter;
				Out.Radius = BodyRadius;
			}
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
		case ECineShotSize::ExtremeCloseUp: return 2.2;
		case ECineShotSize::CloseUp:        return 2.8;
		case ECineShotSize::MediumCloseUp:  return 3.4;
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

	/** Offset from actor origin so tracking focus locks on the head/face when present. */
	FVector TrackingFocusOffset(const AActor* Actor, ECineShotSize Size)
	{
		if (!Actor)
		{
			return FVector::ZeroVector;
		}
		const FVector Interest = ResolveSubjectFraming(Actor, Size).Point;
		return Interest - Actor->GetActorLocation();
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

			FwdVec = StartRot.Vector();
			RightVec = FRotationMatrix(StartRot).GetUnitAxis(EAxis::Y);
		}

		void Sample(double TMove, double TReal, FVector& OutPos, FRotator& OutRot) const
		{
			const double Amount = Seg.MoveAmount;
			double Az = Geo.AzimuthDeg;
			const double El = Geo.ElevationDeg;
			double Dist = Geo.Distance;
			FVector Pos = StartPos;

			switch (Seg.Move)
			{
			case ECineMoveType::Static:
			case ECineMoveType::ZoomIn:   // zooms move the lens, not the camera
			case ECineMoveType::ZoomOut:
				break;

			case ECineMoveType::DollyIn:
			{
				const double D = Amount > 0.0 ? Amount : Dist * 0.5;
				Dist = FMath::Max(Dist - D * TMove, Geo.Radius * 1.05);
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * Dist;
				break;
			}
			case ECineMoveType::DollyOut:
			{
				const double D = Amount > 0.0 ? Amount : Dist;
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * (Dist + D * TMove);
				break;
			}
			case ECineMoveType::OrbitCW:
			case ECineMoveType::OrbitCCW:
			{
				const double Deg = Amount > 0.0 ? Amount : 90.0;
				Az += (Seg.Move == ECineMoveType::OrbitCW ? Deg : -Deg) * TMove;
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * Dist;
				break;
			}
			case ECineMoveType::TruckLeft:
			case ECineMoveType::TruckRight:
			{
				const double D = Amount > 0.0 ? Amount : FMath::Max(Dist * 0.75, 300.0);
				Pos = StartPos + RightVec * (Seg.Move == ECineMoveType::TruckRight ? D : -D) * TMove;
				break;
			}
			case ECineMoveType::CraneUp:
			case ECineMoveType::CraneDown:
			{
				const double D = Amount > 0.0 ? Amount : 300.0;
				Pos = StartPos + FVector::UpVector * (Seg.Move == ECineMoveType::CraneUp ? D : -D) * TMove;
				break;
			}
			case ECineMoveType::Flyover:
			{
				if (bChained && Geo.bHasTarget)
				{
					// Continue from the current position: fly over the target to the
					// mirrored side, arcing up over the subject on the way.
					const FVector End(
						2.0 * Geo.TargetPoint.X - StartPos.X,
						2.0 * Geo.TargetPoint.Y - StartPos.Y,
						StartPos.Z);
					Pos = FMath::Lerp(StartPos, End, TMove);
					Pos.Z += FMath::Sin(TMove * PI) * FMath::Max(Geo.Radius * 1.5, 300.0);
				}
				else if (Geo.bHasTarget)
				{
					const double Travel = Amount > 0.0 ? Amount : FMath::Max(Dist * 2.0, 3000.0);
					const double Height = FMath::Max(Geo.Radius * 2.5, 500.0);
					const FVector AzDir = SphericalOffset(Az, 0.0);
					const FVector PathStart = Geo.TargetPoint + AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
					const FVector PathEnd = Geo.TargetPoint - AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
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
				break; // rotation-only, handled below
			}

			FRotator Rot = StartRot;
			if (bIsPanTilt)
			{
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
			else if (bAim)
			{
				Rot = (Geo.AimPoint - Pos).Rotation();
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

		/** Curved paths and noise can't be represented by two keys; everything else can. */
		bool NeedsDenseKeys() const
		{
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

	void AddTransformKeys(UMovieScene3DTransformSection* Section, const FShotSampler& Sampler, FFrameNumber SegStart, FFrameRate TickResolution, FEulerContinuity& Euler, bool bSkipFirstKey = false)
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
		const int32 SamplesPerSecond = Seg.HandheldIntensity > 0.0f ? 10 : 6;
		const int32 NumKeys = bDense ? FMath::Clamp((int32)(Duration * SamplesPerSecond), 8, 400) + 1 : 2;
		// Dense samples are close enough together that linear interpolation is exact;
		// two-key shots get cubic keys whose auto tangents give the ease for free.
		const bool bLinearKeys = bDense || Seg.Easing == ECineEasing::Linear;

		for (int32 K = 0; K < NumKeys; ++K)
		{
			if (K == 0 && bSkipFirstKey)
			{
				// Continuous take: the previous segment's end key doubles as this segment's start.
				continue;
			}

			const double T = (double)K / (NumKeys - 1);
			const double TMove = (bDense && Seg.Easing == ECineEasing::EaseInOut) ? T * T * (3.0 - 2.0 * T) : T;

			FVector Pos;
			FRotator Rot;
			Sampler.Sample(TMove, T, Pos, Rot);

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

			const FFrameNumber Frame = SegStart + TickResolution.AsFrameTime(T * Seg.DurationSeconds).RoundToFrame();
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

	// Append after whatever camera cuts already exist so re-running extends the edit.
	FFrameNumber Cursor = MovieScene->GetPlaybackRange().GetLowerBoundValue();
	if (UMovieSceneTrack* ExistingCuts = MovieScene->GetCameraCutTrack())
	{
		for (UMovieSceneSection* Section : ExistingCuts->GetAllSections())
		{
			if (Section && Section->HasEndFrame())
			{
				Cursor = FMath::Max(Cursor, Section->GetRange().GetUpperBoundValue());
			}
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateShotsTransaction", "CineDirector: Create Shots"));
	Sequence->Modify();
	MovieScene->Modify();

	UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
	if (Plan.bCreateCameraCuts && !CutTrack)
	{
		CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}
	if (CutTrack)
	{
		CutTrack->Modify();
	}

	if (Plan.bOneContinuousShot)
	{
		// ---- One camera, every move chained into a single unbroken take -------
		const FCineShotSegment& First = Plan.Segments[0];
		const FShotGeometry FirstGeo = ComputeGeometry(First, ViewLoc, ViewRot);
		const FShotSampler FirstSampler(First, FirstGeo, ViewRot);

		FVector StartPos;
		FRotator StartRot;
		FirstSampler.Sample(0.0, 0.0, StartPos, StartRot);

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

		float MaxGrain = 0.0f, MaxVignette = 0.0f, MaxFringe = 0.0f, MaxBloom = 0.0f, MaxFlare = 0.0f;
		bool bTrackingFocusSet = false;
		TArray<TPair<FFrameNumber, float>> FocalKeys;
		TArray<TPair<FFrameNumber, float>> FocusKeys;
		TArray<TPair<const FCineShotSegment*, FFrameNumber>> SegmentStarts;

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

			FVector SegStartPos, SegEndPos;
			FRotator SegStartRot, SegEndRot;
			Sampler.Sample(0.0, 0.0, SegStartPos, SegStartRot);
			Sampler.Sample(1.0, 1.0, SegEndPos, SegEndRot);

			FFrameNumber DurFrames = TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame();
			DurFrames = FMath::Max(DurFrames, FFrameNumber(1));
			const FFrameNumber SegStart = Cursor;
			const FFrameNumber SegEnd = SegStart + DurFrames;

			AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler, /*bSkipFirstKey*/ !bFirst);
			SegmentStarts.Emplace(&Seg, SegStart);

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

			// Focus: rack focus becomes manual-distance keys; otherwise the first
			// tracking request wins for the whole take (offset to head when present).
			if (Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid() && !bTrackingFocusSet)
			{
				const ECineShotSize Size = EffectiveShotSize(Seg);
				FocusKeys.Emplace(SegStart, (float)FVector::Dist(SegStartPos, ActorCenter(Seg.TargetActor.Get(), Size)));
				FocusKeys.Emplace(SegEnd, (float)FVector::Dist(SegEndPos, ActorCenter(Seg.RackFocusToActor.Get(), Size)));
			}
			else if (Seg.bTrackFocus && (Seg.LookAtActor.IsValid() || Seg.TargetActor.IsValid()) && !bTrackingFocusSet && FocusKeys.Num() == 0)
			{
				AActor* FocusActor = Seg.LookAtActor.IsValid() ? Seg.LookAtActor.Get() : Seg.TargetActor.Get();
				Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
				Lens->FocusSettings.TrackingFocusSettings.ActorToTrack = FocusActor;
				Lens->FocusSettings.TrackingFocusSettings.RelativeOffset =
					TrackingFocusOffset(FocusActor, EffectiveShotSize(Seg));
				bTrackingFocusSet = true;
			}

			MaxGrain = FMath::Max(MaxGrain, Seg.FilmGrainIntensity);
			MaxVignette = FMath::Max(MaxVignette, Seg.VignetteIntensity);
			MaxFringe = FMath::Max(MaxFringe, Seg.ChromaticAberrationIntensity);
			MaxBloom = FMath::Max(MaxBloom, Seg.BloomIntensity);
			MaxFlare = FMath::Max(MaxFlare, Seg.LensFlareIntensity);

			FString NoteLine = FString::Printf(TEXT("Move %d:"), MoveIndex);
			if (!Seg.TargetLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(" on '%s'"), *Seg.TargetLabel);
			}
			if (!Seg.RackFocusToLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
			}
			NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
			Result.Notes.Add(MoveTemp(NoteLine));
			for (const FString& Note : Seg.ParseNotes)
			{
				Result.Notes.Add(FString::Printf(TEXT("Move %d: %s"), MoveIndex, *Note));
			}

			UE_LOG(LogCineDirector, Log, TEXT("Take move %d: clause \"%s\", target '%s', %.1fs, frames %d-%d"),
				MoveIndex, *Seg.RawText,
				Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
				Seg.DurationSeconds, SegStart.Value, SegEnd.Value);

			Result.TotalDurationSeconds += Seg.DurationSeconds;
			PrevPos = SegEndPos;
			PrevRot = SegEndRot;
			Cursor = SegEnd;
			++MoveIndex;
		}

		TransformSection->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));

		if (FocusKeys.Num() > 0 && !bTrackingFocusSet)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		}
		else if (!bTrackingFocusSet && FirstGeo.bHasTarget)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.ManualFocusDistance = FVector::Dist(StartPos, FirstGeo.AimPoint);
		}

		FPostProcessSettings& PP = Lens->PostProcessSettings;
		if (MaxGrain > 0.0f) { PP.bOverride_FilmGrainIntensity = true; PP.FilmGrainIntensity = MaxGrain; }
		if (MaxVignette > 0.0f) { PP.bOverride_VignetteIntensity = true; PP.VignetteIntensity = MaxVignette; }
		if (MaxFringe > 0.0f) { PP.bOverride_SceneFringeIntensity = true; PP.SceneFringeIntensity = MaxFringe; }
		if (MaxBloom > 0.0f) { PP.bOverride_BloomIntensity = true; PP.BloomIntensity = MaxBloom; }
		if (MaxFlare > 0.0f) { PP.bOverride_LensFlareIntensity = true; PP.LensFlareIntensity = MaxFlare; }

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
		if (FocusKeys.Num() > 0 && !bTrackingFocusSet)
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

		Result.Notes.Insert(FString::Printf(TEXT("Continuous take: camera '%s', %d moves"), *Label, Plan.Segments.Num()), 0);
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
		const FShotGeometry Geo = ComputeGeometry(Seg, ViewLoc, ViewRot);
		const FShotSampler Sampler(Seg, Geo, ViewRot);

		FVector StartPos, EndPos;
		FRotator StartRot, EndRot;
		Sampler.Sample(0.0, 0.0, StartPos, StartRot);
		Sampler.Sample(1.0, 1.0, EndPos, EndRot);

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

		const bool bRackFocus = Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid();
		if (bRackFocus)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		}
		else if (Seg.bTrackFocus && (Seg.LookAtActor.IsValid() || Seg.TargetActor.IsValid()))
		{
			AActor* FocusActor = Seg.LookAtActor.IsValid() ? Seg.LookAtActor.Get() : Seg.TargetActor.Get();
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
			Lens->FocusSettings.TrackingFocusSettings.ActorToTrack = FocusActor;
			Lens->FocusSettings.TrackingFocusSettings.RelativeOffset =
				TrackingFocusOffset(FocusActor, EffectiveShotSize(Seg));
		}
		else if (Geo.bHasTarget)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.ManualFocusDistance = FVector::Dist(StartPos, Geo.AimPoint);
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
		AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler);

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

		if (bRackFocus)
		{
			const ECineShotSize Size = EffectiveShotSize(Seg);
			const FVector FromPoint = ActorCenter(Seg.TargetActor.Get(), Size);
			const FVector ToPoint = ActorCenter(Seg.RackFocusToActor.Get(), Size);
			AddLensFloatTrack(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"),
				(float)FVector::Dist(StartPos, FromPoint), (float)FVector::Dist(EndPos, ToPoint));
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
		if (!Seg.RackFocusToLabel.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
		}
		NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
		Result.Notes.Add(MoveTemp(NoteLine));
		for (const FString& Note : Seg.ParseNotes)
		{
			Result.Notes.Add(FString::Printf(TEXT("Shot %d: %s"), ShotIndex, *Note));
		}

		UE_LOG(LogCineDirector, Log, TEXT("Shot %d: camera '%s', target '%s', clause \"%s\", %.1fs, frames %d-%d"),
			ShotIndex, *Label,
			Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
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

	Result.bSuccess = true;
	UE_LOG(LogCineDirector, Log, TEXT("Created %d shot(s), %.1fs total, in '%s'."),
		Result.NumShots, Result.TotalDurationSeconds, *Sequence->GetName());
	return Result;
}

#undef LOCTEXT_NAMESPACE
