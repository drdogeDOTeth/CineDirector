import unreal
for cls in ['NiagaraToolset_System','NiagaraToolset_Info','NiagaraExternalEditContext','NiagaraToolset_Component']:
    c=getattr(unreal,cls)
    print('API',cls,[n for n in dir(c) if not n.startswith('_')])
reg=unreal.AssetRegistryHelpers.get_asset_registry()
for a in reg.get_assets_by_class(unreal.TopLevelAssetPath('/Script/Niagara','NiagaraSystem'),True):
    p=str(a.package_name)
    if p.startswith('/Game/') and any(k in p.lower() for k in ['beam','aura','teleport','laser','charge','lightning','energy','magic','burst']):
        print('ASSET',p)
