"""Run through uepy.py. Additive library build; never edits the shot or source clips."""
import unreal, json, math, os

OUT = '/Game/CineDirector/AnimationLibrary/Templar'
REPORT = r'C:\Users\Round\OneDrive\Desktop\UnrealAnimator\CineDirector\Docs\templar_animation_manifest.json'
GROUPS = {
    'Locomotion': 'walk_forward walk_cautious walk_confident walk_tired walk_backward walk_strafe_left walk_strafe_right tiptoe_walk run_jog run_sprint run_backward run_strafe_left run_strafe_right start_sprint stop_from_run turn_left_90 turn_right_90 turn_180'.split(),
    'Traversal': 'climb_stairs_down climb_stairs_up step_over_obstacle crouch_idle crouch_walk_forward crouch_rise_up jump_standing jump_running land_soft forward_roll backward_roll'.split(),
    'Combat': 'magic_sword_slash leap_attack_overhead spin_attack_360 shield_bash shield_block_heavy sidestep_dodge charge_forward_shoulder challenge_stance taunt_come_here enchant_weapon'.split(),
    'Performance': 'idle_neutral idle_alert idle_looking_around idle_relaxed_weight_shift bow_deep_respect salute_military arms_crossed_defiant exhausted_heavy_breathing stumble_recover'.split(),
}
INPLACE = set('walk_forward walk_cautious walk_confident walk_tired walk_backward walk_strafe_left walk_strafe_right tiptoe_walk run_jog run_sprint run_backward run_strafe_left run_strafe_right climb_stairs_down climb_stairs_up crouch_walk_forward'.split())
EAL, AL = unreal.EditorAssetLibrary, unreal.AnimationLibrary
rtg = unreal.load_asset('/Game/CineDirector/Retarget/RTG_Animpack2_to_Templar')
ctrl = unreal.IKRetargeterController.get_controller(rtg)
source = ctrl.get_preview_mesh(unreal.RetargetSourceOrTarget.SOURCE)
target = ctrl.get_preview_mesh(unreal.RetargetSourceOrTarget.TARGET)
assert source and target
target_skeleton = target.get_editor_property('skeleton')
registry = unreal.AssetRegistryHelpers.get_asset_registry()
manifest = {'target_mesh': target.get_path_name(), 'skeleton': target_skeleton.get_path_name(), 'source': 'Existing project Animpack2, adapted with RTG_Animpack2_to_Templar', 'clips': []}

# Validate all inputs before writing any assets.
for names in GROUPS.values():
    for name in names:
        assert EAL.does_asset_exist('/Game/NewAnims/Animpack2/' + name), name

def audit(anim, category, source_path, variant):
    assert anim.get_editor_property('skeleton') == target_skeleton, anim.get_path_name()
    duration = AL.get_sequence_length(anim)
    tracks = AL.get_animation_track_names(anim)
    assert duration > 0 and len(tracks) > 15
    hips = next((str(n) for n in tracks if str(n).lower().split(':')[-1] == 'hips'), None)
    assert hips, [str(n) for n in tracks]
    p0 = AL.get_bone_pose_for_time(anim, hips, 0, False).translation
    p1 = AL.get_bone_pose_for_time(anim, hips, duration, False).translation
    for bone in tracks:
        for t in (0, duration * 0.25, duration * 0.5, duration * 0.75, duration):
            pose = AL.get_bone_pose_for_time(anim, bone, t, False)
            assert all(math.isfinite(v) for v in [pose.translation.x, pose.translation.y, pose.translation.z, pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w]), (anim.get_name(), bone, t)
    entry = dict(asset=anim.get_path_name(), category=category, source=source_path,
                 variant=variant, duration_seconds=round(duration, 4), tracks=len(tracks),
                 hips_bone=hips, hips_displacement_cm=[round(p1.x-p0.x, 4), round(p1.y-p0.y, 4), round(p1.z-p0.z, 4)],
                 visual_review='pending', root_motion=False)
    EAL.set_metadata_tag(anim, 'TemplarLibrary.Source', source_path)
    EAL.set_metadata_tag(anim, 'TemplarLibrary.Variant', variant)
    EAL.set_metadata_tag(anim, 'TemplarLibrary.Category', category)
    assert EAL.save_loaded_asset(anim, only_if_is_dirty=False)
    manifest['clips'].append(entry)
    print('VERIFIED', anim.get_name(), round(duration, 2), len(tracks), entry['hips_displacement_cm'])
    return hips

for group, names in GROUPS.items():
    folder = OUT + '/' + group
    EAL.make_directory(folder)
    pending = [registry.get_asset_by_object_path('/Game/NewAnims/Animpack2/' + n + '.' + n) for n in names if not EAL.does_asset_exist(folder + '/' + n + '_Templar')]
    if pending:
        inputs = unreal.IKRetargetBatchOperationInputs()
        for key, value in dict(assets_to_retarget=pending, source_mesh=source, target_mesh=target, ik_retarget_asset=rtg,
                               suffix='_Templar', target_path=folder, use_source_path=False,
                               include_referenced_assets=False, overwrite_existing_files=False).items():
            inputs.set_editor_property(key, value)
        results = unreal.IKRetargetBatchOperation.run_batch_retarget(inputs)
        assert len(results) == len(pending), (group, len(results), len(pending))
    for name in names:
        anim = unreal.load_asset(folder + '/' + name + '_Templar')
        anim.set_editor_property('enable_root_motion', False)
        hips = audit(anim, group, '/Game/NewAnims/Animpack2/' + name, 'OriginalTravel')
        if name not in INPLACE:
            continue
        dest = OUT + '/InPlace/' + name + '_Templar_IP'
        if not EAL.does_asset_exist(dest):
            EAL.make_directory(OUT + '/InPlace')
            ip = EAL.duplicate_asset(anim.get_path_name(), dest)
        else:
            ip = unreal.load_asset(dest)
        if EAL.get_metadata_tag(ip, 'TemplarLibrary.Variant') != 'LinearTravelRemoved':
            frames = AL.get_num_frames(anim)
            duration = AL.get_sequence_length(anim)
            poses = [AL.get_bone_pose_for_time(anim, hips, duration*i/frames, False) for i in range(frames+1)]
            positions = [p.translation for p in poses]
            rotations = [p.rotation for p in poses]
            scales = [p.scale3d for p in poses]
            start, end = positions[0], positions[-1]
            # Remove endpoint displacement linearly. Preserve bob, sway and all rotation.
            # For stairs, remove vertical travel too: the authored path supplies the drop.
            dz = end.z-start.z if name.startswith('climb_stairs') else 0.0
            new_positions = []
            for i, p in enumerate(positions):
                u = i / (len(positions)-1)
                new_positions.append(unreal.Vector(p.x-(end.x-start.x)*u, p.y-(end.y-start.y)*u, p.z-dz*u))
            controller = ip.controller
            assert controller.set_bone_track_keys(hips, new_positions, rotations, scales, False)
        audit(ip, 'InPlace', anim.get_path_name(), 'LinearTravelRemoved')
        displacement = manifest['clips'][-1]['hips_displacement_cm']
        assert abs(displacement[0]) < .01 and abs(displacement[1]) < .01, displacement
        if name.startswith('climb_stairs'):
            assert abs(displacement[2]) < .01, displacement

with open(REPORT, 'w', encoding='utf-8') as f:
    json.dump(manifest, f, indent=2)
EAL.sync_browser_to_objects([OUT + '/Combat/magic_sword_slash_Templar'])
print('LIBRARY_COMPLETE', len(manifest['clips']), REPORT)
