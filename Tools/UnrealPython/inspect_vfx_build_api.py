import unreal
for cls, names in [(unreal.SubobjectDataSubsystem,['k2_gather_subobject_data_for_blueprint','add_new_subobject','rename_subobject']), (unreal.SubobjectDataBlueprintFunctionLibrary,['get_data','get_object','get_object_for_blueprint']), (unreal.BlueprintEditorLibrary,['compile_blueprint']), (unreal.MaterialEditingLibrary,['create_material_expression','connect_material_expressions','connect_material_property']), (unreal.LevelSequence,['add_spawnable_from_class','add_spawnable_from_instance'])]:
    for n in names: print(cls.__name__, n, getattr(getattr(cls,n,None),'__doc__','NONE'))
print(unreal.AddNewSubobjectParams.__doc__)
print(unreal.CustomInput.__doc__)
