# Attach BP_Aura_Gold to void_4003_Templar inside the level sequence, so the aura
# is parented to the character rather than just parked at his coordinates.
#
# Attaches to the actor ROOT, deliberately not to a bone. The BP's origin sits at
# the character's feet (GroundRing is at rel Z +3, FlameEnvelope +110), so
# attaching to Hips would lift the whole effect ~103 units and leave the ground
# ring hanging in mid air.
import unreal

SEQ = "/Game/TemplarDoge"
CHILD = "BP_Aura_Gold"
PARENT = "void_4003_Templar"

EAL = unreal.EditorAssetLibrary
MSE = unreal.MovieSceneSequenceExtensions

seq = EAL.load_asset(SEQ)
if not seq:
    raise SystemExit("missing %s" % SEQ)

bindings = {str(b.get_display_name()): b for b in seq.get_bindings()}
child = bindings.get(CHILD)
parent = bindings.get(PARENT)
if child is None or parent is None:
    raise SystemExit("need both bindings; found %s" % list(bindings))

# Idempotent: drop any attach track we (or a previous run) already added.
for t in child.get_tracks():
    if isinstance(t, unreal.MovieScene3DAttachTrack):
        unreal.log("removing existing attach track")
        child.remove_track(t)

start = seq.get_playback_start_seconds()
end = seq.get_playback_end_seconds()

track = child.add_track(unreal.MovieScene3DAttachTrack)
sec = track.add_section()
sec.set_range_seconds(start, end)
sec.set_constraint_binding_id(MSE.get_binding_id(seq, parent))

# Empty socket + component => parent to the actor's root component.
sec.set_editor_property("attach_socket_name", "")
sec.set_editor_property("attach_component_name", "")

# Snap location so the aura centres exactly on the character instead of keeping
# the couple of units of hand-placement drift. Rotation/scale stay world so a
# radially symmetric aura never inherits an odd spin.
sec.set_editor_property("attachment_location_rule", unreal.AttachmentRule.SNAP_TO_TARGET)
sec.set_editor_property("attachment_rotation_rule", unreal.AttachmentRule.KEEP_WORLD)
sec.set_editor_property("attachment_scale_rule", unreal.AttachmentRule.KEEP_WORLD)
sec.set_editor_property("detachment_location_rule", unreal.DetachmentRule.KEEP_RELATIVE)
sec.set_editor_property("detachment_rotation_rule", unreal.DetachmentRule.KEEP_RELATIVE)
sec.set_editor_property("detachment_scale_rule", unreal.DetachmentRule.KEEP_RELATIVE)

unreal.log("attached %s -> %s over %.2f..%.2fs" % (CHILD, PARENT, start, end))

EAL.save_loaded_asset(seq, False)
unreal.log("saved %s" % SEQ)

# Report the result
for t in child.get_tracks():
    for s in t.get_sections():
        unreal.log("  %s  %.2f..%.2fs  socket=%r" % (
            t.get_class().get_name().replace("MovieScene", ""),
            s.get_start_frame_seconds(), s.get_end_frame_seconds(),
            str(s.get_editor_property("attach_socket_name"))))
