import unreal
seq=unreal.load_asset('/Game/CineDirector/VFX/Anime/Sequences/LS_Aura_Gold')
for b in seq.get_bindings():
    print('BIND',b.get_name(),'PARENT',b.get_parent().get_name())
    for t in b.get_tracks():
        for s in t.get_sections():
            print(s.get_class().get_name())
            if hasattr(s,'add_scalar_parameter_key'): print(s.add_scalar_parameter_key.__doc__)
for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if a.get_actor_label()=='CDVFX_SEQUENCE_BUILD': unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(a)
