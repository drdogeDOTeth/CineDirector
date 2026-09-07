"""Add dances and DBZ-inspired motion adaptations; leave all existing shots alone."""
import unreal, json, math

ROOT = '/Game/CineDirector/AnimationLibrary/Templar'
REPORT = r'C:\Users\Round\OneDrive\Desktop\UnrealAnimator\CineDirector\Docs\templar_emotes_manifest.json'
EAL, AL = unreal.EditorAssetLibrary, unreal.AnimationLibrary
retargeter=unreal.load_asset('/Game/CineDirector/Retarget/RTG_Animpack2_to_Templar')
rc=unreal.IKRetargeterController.get_controller(retargeter)
source_mesh=rc.get_preview_mesh(unreal.RetargetSourceOrTarget.SOURCE)
target_mesh=rc.get_preview_mesh(unreal.RetargetSourceOrTarget.TARGET)
skeleton=target_mesh.get_editor_property('skeleton')
specs=[
 ('DanceEmotes','EMOTE_HouseDance_A','/Game/Characters/Mannequins/DrDogeVoid/House_Dancing1','House dance'),
 ('DanceEmotes','EMOTE_HouseDance_B','/Game/Characters/Mannequins/DrDogeVoid/House_Dancing2','House dance variation'),
 ('DanceEmotes','EMOTE_HipHop_Locking','/Game/Characters/Mannequins/DrDogeVoid/Locking_Hip_Hop_Dance','Hip-hop locking'),
 ('DanceEmotes','EMOTE_HipHop_Slide_A','/Game/Characters/Mannequins/DrDogeVoid/Slide_Hip_Hop_Dance','Sliding hip-hop'),
 ('DanceEmotes','EMOTE_HipHop_Slide_B','/Game/Characters/Mannequins/DrDogeVoid/Slide_Hip_Hop_Dance_2','Sliding hip-hop variation'),
 ('DanceEmotes','EMOTE_ArmWave','/Game/NewAnims/Animpack2/dance_arm_wave','Arm-wave dance'),
 ('DanceEmotes','EMOTE_Groove','/Game/NewAnims/Animpack2/dance_simple_groove','Relaxed groove'),
 ('DanceEmotes','EMOTE_Victory','/Game/NewAnims/Animpack2/fist_pump_victory','Victory celebration'),
 ('DBZInspired','DBZ_PowerUp','/Game/NewAnims/Animpack2/power_charge_buildup','Power-up charge'),
 ('DBZInspired','DBZ_KiBlast','/Game/NewAnims/Animpack2/cast_fireball','Ki-blast casting motion'),
 ('DBZInspired','DBZ_BlastRelease','/Game/NewAnims/Animpack2/cast_wind_blast','Energy-blast release motion'),
 ('DBZInspired','DBZ_EnergyGather','/Game/NewAnims/Animpack2/absorb_energy','Energy gathering'),
 ('DBZInspired','DBZ_Levitate','/Game/NewAnims/Animpack2/cast_levitate_rising','Rising levitation'),
 ('DBZInspired','DBZ_InstantTransmission','/Game/NewAnims/Animpack2/cast_teleport_vanish','Teleport preparation gesture'),
 ('DBZInspired','DBZ_GroundSmash','/Game/NewAnims/Animpack2/ground_slam_both_fists','Two-fist ground smash'),
]
for group,name,src,label in specs:
    assert EAL.does_asset_exist(src), src
manifest=[]
for group,name,src,label in specs:
    folder=ROOT+'/'+group
    dest=folder+'/'+name+'_Templar'
    EAL.make_directory(folder)
    source=unreal.load_asset(src)
    if not EAL.does_asset_exist(dest):
        if source.get_editor_property('skeleton')==skeleton:
            anim=EAL.duplicate_asset(src,dest)
        else:
            inputs=unreal.IKRetargetBatchOperationInputs()
            for key,value in dict(assets_to_retarget=[EAL.find_asset_data(src)],source_mesh=source_mesh,target_mesh=target_mesh,
                                  ik_retarget_asset=retargeter,search=source.get_name(),replace=name+'_Templar',
                                  target_path=folder,use_source_path=False,include_referenced_assets=False,
                                  overwrite_existing_files=False).items():
                inputs.set_editor_property(key,value)
            results=unreal.IKRetargetBatchOperation.run_batch_retarget(inputs)
            assert len(results)==1
            anim=unreal.load_asset(dest)
    else:
        anim=unreal.load_asset(dest)
    assert anim and anim.get_editor_property('skeleton')==skeleton
    anim.set_editor_property('enable_root_motion',False)
    length=AL.get_sequence_length(anim)
    tracks=AL.get_animation_track_names(anim)
    assert length>0 and len(tracks)>15
    for bone in tracks:
        for fraction in (0,.25,.5,.75,1):
            pose=AL.get_bone_pose_for_time(anim,bone,length*fraction,False)
            assert all(math.isfinite(v) for v in [pose.translation.x,pose.translation.y,pose.translation.z,pose.rotation.x,pose.rotation.y,pose.rotation.z,pose.rotation.w])
    EAL.set_metadata_tag(anim,'TemplarLibrary.Source',src)
    EAL.set_metadata_tag(anim,'TemplarLibrary.Category',group)
    EAL.set_metadata_tag(anim,'TemplarLibrary.Description',label+'; adapted existing motion; no VFX')
    assert EAL.save_loaded_asset(anim,only_if_is_dirty=False)
    manifest.append(dict(asset=dest,source=src,label=label,seconds=length,tracks=len(tracks),visual_review='pending',effects=False))
    print('SAVED',dest,round(length,2),'seconds',len(tracks),'tracks')
with open(REPORT,'w',encoding='utf-8') as f:
    json.dump(manifest,f,indent=2)
print('EMOTES_COMPLETE',len(manifest))
