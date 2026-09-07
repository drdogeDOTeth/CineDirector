import unreal
R='/Game/CineDirector/VFX/Anime'
E=unreal.EditorAssetLibrary
A=unreal.AssetToolsHelpers.get_asset_tools()
sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
def scalar_key(section,name,frame,value,interpolation):
    info=unreal.MaterialParameterInfo()
    info.set_editor_property('name',name)
    section.add_scalar_parameter_key(info,unreal.FrameNumber(frame*800),value,'','',interpolation)
specs=[('Aura_Gold',120),('Aura_Blue',120),('Aura_Violet',120),('EnergyBeam_Blue',90),('EnergyBeam_Violet',90),('Teleport_Blue',24),('Teleport_Violet',24)]
for name,end in specs:
    path=R+'/Sequences/LS_'+name
    if E.does_asset_exist(path):
        seq=unreal.load_asset(path)
        if E.get_metadata_tag(seq,'AnimeVFX.Complete')=='1':
            print('EXISTS',path)
            continue
        for old_binding in seq.get_bindings(): old_binding.remove()
    else:
        seq=A.create_asset('LS_'+name,R+'/Sequences',unreal.LevelSequence,unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(30,1))
    seq.set_tick_resolution_directly(unreal.FrameRate(24000,1))
    seq.set_playback_start(0)
    seq.set_playback_end(end)
    seq.set_view_range_start(-.1)
    seq.set_view_range_end(end/30+.1)
    cls=E.load_blueprint_class(R+'/Effects/BP_'+name)
    actor=sub.spawn_actor_from_class(cls,unreal.Vector(0,0,0),transient=True)
    actor.set_actor_label('CDVFX_SEQUENCE_BUILD')
    binding=seq.add_spawnable_from_instance(actor)
    assert binding.is_valid()
    binding.set_name(name)
    template=binding.get_object_template()
    assert template
    components=actor.get_components_by_class(unreal.StaticMeshComponent)
    assert len(components)>=3,(name,'no template meshes')
    if name.startswith('Aura'):
        envelope=[(0,0.),(12,.18),(45,.7),(100,.7),(119,0.)]
    elif name.startswith('EnergyBeam'):
        envelope=[(0,0.),(3,.7),(70,.7),(89,0.)]
    else:
        envelope=[(0,0.),(3,.9),(7,.7),(15,.35),(23,0.)]
    for comp in components:
        cb=seq.add_possessable(comp)
        cb.set_parent(binding)
        cb.set_name(comp.get_name())
        track=cb.add_track(unreal.MovieSceneComponentMaterialTrack)
        track.set_material_index(0)
        track.set_display_name('Energy timing | Material 0')
        section=track.add_section()
        section.set_range(0,end)
        for frame,value in envelope:
            scalar_key(section,'Opacity',frame,value,unreal.MovieSceneKeyInterpolation.LINEAR)
        scalar_key(section,'UseManualTime',0,1.,unreal.MovieSceneKeyInterpolation.CONSTANT)
        scalar_key(section,'ManualTime',0,0.,unreal.MovieSceneKeyInterpolation.LINEAR)
        scalar_key(section,'ManualTime',end-1,(end-1)/30.,unreal.MovieSceneKeyInterpolation.LINEAR)
    transform=binding.add_track(unreal.MovieScene3DTransformTrack)
    transform.set_display_name('Effect position / aim / size')
    section=transform.add_section()
    section.set_range(0,end)
    for i,c in enumerate(section.get_all_channels()):
        c.set_default(1. if i in [6,7,8] else 0.)
    for extra in list(seq.get_bindings()):
        if extra.get_id()!=binding.get_id() and not extra.get_parent().is_valid():
            extra.remove()
    sub.destroy_actor(actor)
    E.set_metadata_tag(seq,'AnimeVFX.Complete','1')
    assert E.save_loaded_asset(seq,False)
    print('SEQUENCE_SAVED',path,len(components),'animated material bindings',end,'frames')
print('SEQUENCE_PRESETS_COMPLETE')
