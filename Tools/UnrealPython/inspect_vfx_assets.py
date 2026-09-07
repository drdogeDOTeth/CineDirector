import unreal
reg=unreal.AssetRegistryHelpers.get_asset_registry()
for cls in ['NiagaraSystem','ParticleSystem','Blueprint']:
    module='/Script/Niagara' if cls=='NiagaraSystem' else '/Script/Engine'
    for a in reg.get_assets_by_class(unreal.TopLevelAssetPath(module,cls),True):
        path=str(a.package_name)
        if path.startswith('/Game/') and (cls!='Blueprint' or any(k in path.lower() for k in ['beam','aura','teleport'])):
            print('VFX',cls,path)
print('PYTHON_APIS', [n for n in dir(unreal) if any(k in n for k in ['Niagara','BlueprintEditor','SubobjectData'])])
