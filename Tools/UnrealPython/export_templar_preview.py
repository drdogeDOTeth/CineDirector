import unreal, os
folder = r'C:\Users\Round\OneDrive\Desktop\UnrealAnimator\CineDirector\AnimationPreviews'
os.makedirs(folder, exist_ok=True)
anim = unreal.load_asset('/Game/CineDirector/AnimationLibrary/Templar/Combat/magic_sword_slash_Templar')
task = unreal.AssetExportTask()
task.object = anim
task.filename = folder + '/magic_sword_slash_Templar.fbx'
task.automated = True
task.prompt = False
task.replace_identical = True
task.exporter = unreal.AnimSequenceExporterFBX()
task.options = unreal.FbxExportOption()
task.options.set_editor_property('export_preview_mesh', True)
assert unreal.Exporter.run_asset_export_task(task)
print('EXPORTED', task.filename)
