"""Build original procedural mesh VFX. No external assets or level edits."""
import unreal, json
ROOT='/Game/CineDirector/VFX/Anime'
E=unreal.EditorAssetLibrary
M=unreal.MaterialEditingLibrary
A=unreal.AssetToolsHelpers.get_asset_tools()
S=unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
F=unreal.SubobjectDataBlueprintFunctionLibrary
manifest=[]

MASKS={
'Aura': '''float t=lerp(Time*Speed+Phase,ManualTime,saturate(UseManualTime));
float u=UV.x*6.2831853; float v=UV.y;
float waves=sin(u*9+sin(u*3-t*2)*1.6+t*.6)*.5+.5;
float tongues=pow(waves,3)*.28+pow(sin(u*17-t)*.5+.5,7)*.15;
float lower=smoothstep(.02,.20,v); float upper=1-smoothstep(.53+tongues,.86+tongues,v);
float bands=pow(sin(v*38-t*7+sin(u*5+t)*2)*.5+.5,5);
float edge=pow(1-saturate(abs(dot(normalize(Normal),normalize(View)))),1.6);
return saturate(lower*upper*(.18+bands*.62+edge*.8)*(0.85+0.15*sin(t*11)))*Opacity;''',
'Beam': '''float t=lerp(Time*Speed+Phase,ManualTime,saturate(UseManualTime));
float u=UV.x*6.2831853; float v=UV.y;
float streak=pow(sin(u*11+sin(v*23-t*8)*1.2+t*2)*.5+.5,5);
float pulse=pow(sin(v*95-t*20+u*3)*.5+.5,10);
float ends=smoothstep(0,.025,v)*(1-smoothstep(.975,1,v));
return saturate((.24+.65*streak+.35*pulse)*ends)*Opacity;''',
'Ring': '''float t=lerp(Time*Speed+Phase,ManualTime,saturate(UseManualTime));
float2 p=(UV-.5)*2; float r=length(p); float angle=atan2(p.y,p.x);
float ring=exp(-pow((r-.73)/.025,2))+exp(-pow((r-.87)/.012,2))*.55;
float streak=pow(sin(angle*24+t*4)*.5+.5,24)*smoothstep(.25,.68,r)*(1-smoothstep(.7,.98,r));
float inner=exp(-r*r*16)*.3;
return saturate(ring+streak*.8+inner)*Opacity;''',
'Orb': '''float t=lerp(Time*Speed+Phase,ManualTime,saturate(UseManualTime));
float rim=pow(1-saturate(abs(dot(normalize(Normal),normalize(View)))),2);
float veins=pow(sin(UV.x*65+sin(UV.y*40-t*6)*2+t*4)*.5+.5,10);
return saturate(.12+rim*.65+veins*.45)*Opacity;''',
'Teleport': '''float t=lerp(Time*Speed+Phase,ManualTime,saturate(UseManualTime));
float u=UV.x*6.2831853; float v=UV.y;
float streak=pow(sin(u*21+sin(u*5)*2)*.5+.5,20);
float sweep=pow(sin(v*16-t*13+u)*.5+.5,8);
float fade=smoothstep(.02,.15,v)*(1-smoothstep(.83,.98,v));
return saturate((streak*.85+sweep*.6)*fade)*Opacity;'''
}

def material(kind):
    path=ROOT+'/Materials/M_Anime_'+kind
    if E.does_asset_exist(path): return unreal.load_asset(path)
    mat=A.create_asset('M_Anime_'+kind,ROOT+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
    mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property('two_sided',True)
    nodes={}
    for name,cls in [('UV',unreal.MaterialExpressionTextureCoordinate),('Time',unreal.MaterialExpressionTime),('Normal',unreal.MaterialExpressionPixelNormalWS),('View',unreal.MaterialExpressionCameraVectorWS)]:
        nodes[name]=M.create_material_expression(mat,cls,-900,len(nodes)*110)
    for name,val in [('Speed',1.),('Phase',0.),('ManualTime',0.),('UseManualTime',0.),('Opacity',.7),('Intensity',5.)]:
        node=M.create_material_expression(mat,unreal.MaterialExpressionScalarParameter,-650,len(nodes)*70)
        node.set_editor_property('parameter_name',name)
        node.set_editor_property('default_value',val)
        nodes[name]=node
    color=M.create_material_expression(mat,unreal.MaterialExpressionVectorParameter,-400,-200)
    color.set_editor_property('parameter_name','Color')
    color.set_editor_property('default_value',unreal.LinearColor(.04,.55,1,1))
    mask=M.create_material_expression(mat,unreal.MaterialExpressionCustom,0,150)
    mask.set_editor_property('code',MASKS[kind])
    mask.set_editor_property('output_type',unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    mask.set_editor_property('description','Procedural '+kind+' | manual time supports Sequencer')
    inputs=[]
    for name in nodes:
        if name=='Intensity': continue
        inp=unreal.CustomInput()
        inp.set_editor_property('input_name',name)
        inputs.append(inp)
    mask.set_editor_property('inputs',inputs)
    for name,node in nodes.items():
        if name!='Intensity': assert M.connect_material_expressions(node,'',mask,name)
    emit=M.create_material_expression(mat,unreal.MaterialExpressionMultiply,0,-180)
    assert M.connect_material_expressions(color,'',emit,'A')
    assert M.connect_material_expressions(nodes['Intensity'],'',emit,'B')
    assert M.connect_material_property(emit,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    assert M.connect_material_property(mask,'',unreal.MaterialProperty.MP_OPACITY)
    M.recompile_material(mat)
    assert E.save_loaded_asset(mat,False)
    return mat

masters={k:material(k) for k in MASKS}
palette={'Gold':(1,.38,.025),'Blue':(.015,.42,1),'Violet':(.45,.035,1),'White':(.65,.85,1)}
def instance(kind,color,intensity=5,opacity=.7):
    name='MI_'+kind+'_'+color
    path=ROOT+'/Materials/'+name
    if E.does_asset_exist(path): return unreal.load_asset(path)
    mi=A.create_asset(name,ROOT+'/Materials',unreal.MaterialInstanceConstant,unreal.MaterialInstanceConstantFactoryNew())
    M.set_material_instance_parent(mi,masters[kind])
    M.set_material_instance_vector_parameter_value(mi,'Color',unreal.LinearColor(*palette[color],1))
    M.set_material_instance_scalar_parameter_value(mi,'Intensity',intensity)
    M.set_material_instance_scalar_parameter_value(mi,'Opacity',opacity)
    assert E.save_loaded_asset(mi,False)
    return mi

def blueprint(name,parts,description):
    path=ROOT+'/Effects/'+name
    if E.does_asset_exist(path):
        manifest.append(dict(asset=path,description=description))
        return
    factory=unreal.BlueprintFactory()
    factory.set_editor_property('parent_class',unreal.Actor)
    bp=A.create_asset(name,ROOT+'/Effects',unreal.Blueprint,factory)
    handles=S.k2_gather_subobject_data_for_blueprint(bp)
    parent=handles[0]
    for component_name,shape,kind,color,loc,rot,scale in parts:
        params=unreal.AddNewSubobjectParams(parent_handle=parent,new_class=unreal.StaticMeshComponent,blueprint_context=bp)
        handle,reason=S.add_new_subobject(params)
        assert not str(reason), str(reason)
        S.rename_subobject(handle,component_name)
        comp=F.get_object_for_blueprint(F.get_data(handle),bp)
        assert comp
        comp.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/'+shape))
        comp.set_material(0,instance(kind,color))
        comp.set_editor_property('relative_location',unreal.Vector(*loc))
        comp.set_editor_property('relative_rotation',unreal.Rotator(pitch=rot[0],yaw=rot[1],roll=rot[2]))
        comp.set_editor_property('relative_scale3d',unreal.Vector(*scale))
        comp.set_editor_property('cast_shadow',False)
        comp.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        comp.set_editor_property('mobility',unreal.ComponentMobility.MOVABLE)
    assert unreal.BlueprintEditorLibrary.compile_blueprint(bp), name
    E.set_metadata_tag(bp,'AnimeVFX.Description',description)
    assert E.save_loaded_asset(bp,False)
    manifest.append(dict(asset=path,description=description,components=len(parts)))
    print('BUILT',name,len(parts),'components')

for color in ['Gold','Blue','Violet']:
    blueprint('BP_Aura_'+color,[
        ('FlameEnvelope','Sphere','Aura',color,(0,0,110),(0,0,0),(1.65,1.65,2.6)),
        ('InnerEnergy','Sphere','Orb',color,(0,0,100),(0,0,0),(1.2,1.2,2.1)),
        ('GroundRing','Plane','Ring',color,(0,0,3),(0,0,0),(2.5,2.5,2.5)),
    ],color+' power aura. Origin at feet; attach to actor root. Scale to fit.')
for color in ['Blue','Violet']:
    blueprint('BP_EnergyBeam_'+color,[
        ('BeamOuter','Cylinder','Beam',color,(500,0,0),(90,0,0),(.65,.65,10)),
        ('BeamCore','Cylinder','Beam','White',(500,0,0),(90,0,0),(.18,.18,10)),
        ('ChargeOrb','Sphere','Orb',color,(0,0,0),(0,0,0),(1.1,1.1,1.1)),
        ('ImpactOrb','Sphere','Orb',color,(1000,0,0),(0,0,0),(1.6,1.6,1.6)),
        ('MuzzleRing','Plane','Ring',color,(0,0,0),(90,0,0),(1.9,1.9,1.9)),
    ],color+' beam. Origin at muzzle; +X forward; base length 1000 cm. Scale X adjusts length.')
    blueprint('BP_Teleport_'+color,[
        ('SpeedLines','Sphere','Teleport',color,(0,0,110),(0,0,0),(1.5,1.5,2.7)),
        ('FlashShell','Sphere','Orb',color,(0,0,100),(0,0,0),(1.05,1.05,2.2)),
        ('GroundShockRing','Plane','Ring',color,(0,0,3),(0,0,0),(3.4,3.4,3.4)),
        ('UpperShockRing','Plane','Ring',color,(0,0,185),(0,0,0),(2.1,2.1,2.1)),
    ],color+' teleport VFX. Origin at feet; use short spawn/visibility window. Does not move/hide characters itself.')
with open(r'C:\Users\Round\OneDrive\Desktop\UnrealAnimator\CineDirector\Docs\anime_vfx_manifest.json','w') as f:
    json.dump(manifest,f,indent=2)
E.sync_browser_to_objects([ROOT+'/Effects/BP_Aura_Gold'])
print('ANIME_VFX_COMPLETE',len(manifest))
