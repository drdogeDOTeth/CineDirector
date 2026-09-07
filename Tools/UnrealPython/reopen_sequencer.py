import unreal
lib=unreal.LevelSequenceEditorBlueprintLibrary
seq=lib.get_current_level_sequence()
assert seq and seq.get_path_name()=='/Game/NewTemplar.NewTemplar'
time=lib.get_current_time()
# Rebuild the editor widgets against the same in-memory sequence; no reload/discard.
lib.close_level_sequence()
lib.open_level_sequence(seq)
lib.set_current_time(time)
lib.refresh_current_level_sequence()
print('REOPENED',seq.get_path_name())
