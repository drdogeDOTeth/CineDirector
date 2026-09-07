# Parent BP_Aura_Gold to the Templar in the LEVEL, not just inside the sequence.
#
# A MovieScene3DAttachTrack only binds while Sequencer is evaluating; in the
# viewport the two actors stay independent, so moving the character leaves the
# aura behind. A real actor attachment is what makes it follow everywhere -- in
# the outliner, in the viewport, and at runtime.
#
# Root component, no socket: the BP's origin is at the character's feet
# (GroundRing rel Z +3), so a Hips socket would lift it ~103 units.
import unreal

CHILD = "BP_Aura_Gold"
PARENT = "void_4003_Templar"

aes = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = {str(a.get_actor_label()): a for a in aes.get_all_level_actors()}
child, parent = actors.get(CHILD), actors.get(PARENT)
if not child or not parent:
    raise SystemExit("missing actor; have %s" % [k for k in actors if "Aura" in k or "Templar" in k])

cur = child.get_attach_parent_actor()
unreal.log("before: %s attached to %s" % (CHILD, cur.get_actor_label() if cur else "NOTHING"))

# Snap to the character so the aura is exactly centred, then keep that relation.
child.set_actor_location_and_rotation(
    parent.get_actor_location(), parent.get_actor_rotation(), False, True)
child.attach_to_actor(
    parent,
    "",                                   # no socket -> root component
    unreal.AttachmentRule.KEEP_WORLD,
    unreal.AttachmentRule.KEEP_WORLD,
    unreal.AttachmentRule.KEEP_WORLD,
    False)

now = child.get_attach_parent_actor()
unreal.log("after:  %s attached to %s" % (CHILD, now.get_actor_label() if now else "NOTHING"))

# Prove it follows: nudge the parent, read the child, then put the parent back.
p0 = parent.get_actor_location()
c0 = child.get_actor_location()
probe = unreal.Vector(p0.x + 500.0, p0.y + 300.0, p0.z + 120.0)
parent.set_actor_location(probe, False, True)
c1 = child.get_actor_location()
parent.set_actor_location(p0, False, True)
c2 = child.get_actor_location()

unreal.log("parent moved (+500, +300, +120):")
unreal.log("  child before (%.1f, %.1f, %.1f)" % (c0.x, c0.y, c0.z))
unreal.log("  child during (%.1f, %.1f, %.1f)  delta=(%.1f, %.1f, %.1f)" % (
    c1.x, c1.y, c1.z, c1.x - c0.x, c1.y - c0.y, c1.z - c0.z))
unreal.log("  child after  (%.1f, %.1f, %.1f)" % (c2.x, c2.y, c2.z))
unreal.log("FOLLOWS: %s" % (abs(c1.x - c0.x - 500.0) < 1.0 and
                            abs(c1.y - c0.y - 300.0) < 1.0 and
                            abs(c1.z - c0.z - 120.0) < 1.0))

unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
unreal.log("saved")
