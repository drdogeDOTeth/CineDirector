import unreal
R='/Game/CineDirector/VFX/Anime'
world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for name,end in [('Aura_Gold',120),('Aura_Blue',120),('Aura_Violet',120),('EnergyBeam_Blue',90),('EnergyBeam_Violet',90),('Teleport_Blue',24),('Teleport_Violet',24)]:
    seq=unreal.load_asset(R+'/Sequences/LS_'+name)
    cls=unreal.EditorAssetLibrary.load_blueprint_class(R+'/Effects/BP_'+name)
    actor=sub.spawn_actor_from_class(cls,unreal.Vector(200000,200000,200000),transient=True)
    try:
        count=0
        for b in seq.get_bindings():
            if b.get_parent().is_valid():
                objects=seq.locate_bound_objects(b,actor)
                assert len(objects)==1,(name,b.get_name(),objects)
                assert isinstance(objects[0],unreal.StaticMeshComponent)
                count+=1
                for track in b.get_tracks():
                    for section in track.get_sections():
                        channels=section.get_all_channels()
                        assert len(channels)==3,(b.get_name(),len(channels))
                        print('CHANNEL',name,b.get_name(),[(str(c.get_name()),len(c.get_keys())) for c in channels])
        assert count>=3
        print('BINDINGS_VERIFIED',name,count)
    finally:
        sub.destroy_actor(actor)
print('ALL_PRESET_BINDINGS_VERIFIED')
