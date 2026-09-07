import unreal, os
ROOT='/Game/CineDirector/VFX/Anime'
OUT=r'C:\Users\Round\OneDrive\Desktop\UnrealAnimator\CineDirector\AnimationPreviews\VFX'
os.makedirs(OUT,exist_ok=True)
sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
origin=unreal.Vector(200000,200000,200000)
spawned=[]
try:
    for name in ['BP_Aura_Gold','BP_EnergyBeam_Blue','BP_Teleport_Violet']:
        cls=unreal.EditorAssetLibrary.load_blueprint_class(ROOT+'/Effects/'+name)
        actor=sub.spawn_actor_from_class(cls,origin,transient=True)
        spawned.append(actor)
        actor.set_actor_label('CDVFX_PREVIEW_'+name)
        for comp in actor.get_components_by_class(unreal.StaticMeshComponent):
            print(name,comp.get_name(),'SCALE',comp.get_world_scale(),'LOC',comp.get_world_location()-origin)
            mid=comp.create_dynamic_material_instance(0)
            mid.set_scalar_parameter_value('UseManualTime',1.)
            mid.set_scalar_parameter_value('ManualTime',.65)
        beam='Beam' in name
        center=origin+unreal.Vector(500 if beam else 0,0,0 if beam else 110)
        camloc=center+unreal.Vector(250,-1300,450 if beam else 130)
        capture=sub.spawn_actor_from_class(unreal.SceneCapture2D,camloc,unreal.MathLibrary.find_look_at_rotation(camloc,center),transient=True)
        spawned.append(capture)
        c=capture.get_component_by_class(unreal.SceneCaptureComponent2D)
        c.set_editor_property('projection_type',unreal.CameraProjectionMode.ORTHOGRAPHIC)
        c.set_editor_property('ortho_width',1400 if beam else 570)
        c.set_editor_property('capture_source',unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        c.set_editor_property('primitive_render_mode',unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
        c.show_only_actor_components(actor)
        pp=c.get_editor_property('post_process_settings')
        pp.set_editor_property('override_auto_exposure_method',True)
        pp.set_editor_property('auto_exposure_method',unreal.AutoExposureMethod.AEM_MANUAL)
        pp.set_editor_property('override_auto_exposure_apply_physical_camera_exposure',True)
        pp.set_editor_property('auto_exposure_apply_physical_camera_exposure',False)
        pp.set_editor_property('override_auto_exposure_bias',True)
        pp.set_editor_property('auto_exposure_bias',0.)
        pp.set_editor_property('override_bloom_intensity',True)
        pp.set_editor_property('bloom_intensity',.7)
        pp.set_editor_property('override_scene_fringe_intensity',True)
        pp.set_editor_property('scene_fringe_intensity',0.)
        c.set_editor_property('post_process_settings',pp)
        rt=unreal.RenderingLibrary.create_render_target2d(world,960,600,unreal.TextureRenderTargetFormat.RTF_RGBA8)
        c.set_editor_property('texture_target',rt)
        c.capture_scene()
        unreal.RenderingLibrary.export_render_target(world,rt,OUT,name+'.png')
        print('PREVIEW',name)
finally:
    for a in reversed(spawned): sub.destroy_actor(a)
