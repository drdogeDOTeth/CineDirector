import unreal
seq = unreal.LevelSequenceEditorBlueprintLibrary.get_current_level_sequence()
print('CURRENT_SEQUENCE', seq)
if seq:
    print('RANGE', seq.get_playback_start(), seq.get_playback_end(), 'VIEW', seq.get_view_range_start(), seq.get_view_range_end())
    print('TIME', unreal.LevelSequenceEditorBlueprintLibrary.get_current_time())
for name in unreal.LevelSequenceEditorBlueprintLibrary.get_track_filter_names():
    print('FILTER', name, unreal.LevelSequenceEditorBlueprintLibrary.is_track_filter_active(name), unreal.LevelSequenceEditorBlueprintLibrary.is_track_filter_enabled(name))
if seq:
    for b in seq.get_bindings():
        print('BINDING', b.get_name(), b.get_id())
        for t in b.get_tracks():
            print(' TRACK', t.get_class().get_name(), t.get_display_name())
            for s in t.get_sections():
                print('  SECTION', s.get_class().get_name(), 'ACTIVE', s.is_active())
                if isinstance(s, unreal.MovieSceneSkeletalAnimationSection):
                    print('  RANGE', s.get_start_frame(), s.get_end_frame())
                    print('  ANIMATION', s.get_editor_property('params').animation)
print('FILTER_APIS', [n for n in dir(unreal.LevelSequenceEditorBlueprintLibrary) if any(k in n for k in ['filter','binding','track','refresh'])])
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if 'templar' in a.get_actor_label().lower():
        print('ACTOR', a.get_actor_label())
        for c in a.get_components_by_class(unreal.SkeletalMeshComponent):
            print(' MESH', c.get_name(), c.get_editor_property('animation_mode'))
print('DANCE_SOURCES')
reg=unreal.AssetRegistryHelpers.get_asset_registry()
for a in reg.get_assets_by_class(unreal.TopLevelAssetPath('/Script/Engine','AnimSequence'), True):
    p=str(a.package_name)
    if p.startswith('/Game/Characters/Mannequins/DrDogeVoid/') and any(k in p.lower() for k in ['dance','dancing','hip_hop']):
        obj=a.get_asset()
        print(p, obj.get_editor_property('skeleton').get_path_name())
