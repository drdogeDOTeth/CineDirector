# Remove every temporary actor these preview passes may have left in the level.
import unreal

aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
PREFIXES = ("CDTEST_", "CDPREVIEW_", "CDMARK_")

removed = 0
for a in aes.get_all_level_actors():
    label = str(a.get_actor_label())
    if label.startswith(PREFIXES) or isinstance(a, unreal.SceneCapture2D):
        unreal.log("removing %s (%s)" % (label, a.get_class().get_name()))
        aes.destroy_actor(a)
        removed += 1

unreal.log("removed %d temp actor(s)" % removed)

for a in aes.get_all_level_actors():
    if "templar" in str(a.get_actor_label()).lower():
        try:
            a.set_is_temporarily_hidden_in_editor(False)
        except Exception:
            pass
        loc = a.get_actor_location()
        rot = a.get_actor_rotation()
        unreal.log("%-22s loc=(%.1f, %.1f, %.1f) yaw=%.1f visible" % (
            str(a.get_actor_label()), loc.x, loc.y, loc.z, rot.yaw))
