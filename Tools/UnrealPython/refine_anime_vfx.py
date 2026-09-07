import unreal
root='/Game/CineDirector/VFX/Anime'
S=unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
F=unreal.SubobjectDataBlueprintFunctionLibrary
for color in ['Blue','Violet']:
    bp=unreal.load_asset(root+'/Effects/BP_EnergyBeam_'+color)
    for handle in S.k2_gather_subobject_data_for_blueprint(bp):
        obj=F.get_object_for_blueprint(F.get_data(handle),bp)
        if isinstance(obj,unreal.StaticMeshComponent) and any(n in obj.get_name() for n in ['BeamOuter','BeamCore','MuzzleRing']):
            obj.set_editor_property('relative_rotation',unreal.Rotator(pitch=90,yaw=0,roll=0))
    assert unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    assert unreal.EditorAssetLibrary.save_loaded_asset(bp,False)
for cls,names in [(unreal.MovieSceneComponentMaterialTrack,['set_material_index']), (unreal.MovieSceneParameterSection,['add_scalar_parameter_key']), (unreal.MovieSceneBindingProxy,['get_object_template','set_parent'])]:
    for n in names: print('API',n,getattr(getattr(cls,n,None),'__doc__','NONE'))
