# blender -b -P auto_intro.py -- <model.glb> <out folder> [res percent]
# Generates a POTG intro for any champion: picks the recall, finds its big move and end pose, builds pacing + camera
import bpy, sys, json, math, os, re
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
glb, out = args[:2]
res = int(args[2]) if len(args) > 2 else 100
FPS, TOTAL = 30, 150

bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
scn.render.fps = FPS  # the importer converts clip seconds at the scene rate, so set it before importing
bpy.ops.import_scene.gltf(filepath=glb)
scn.frame_start, scn.frame_end = 1, TOTAL
arm = next(o for o in scn.objects if o.type == "ARMATURE")
for t in list(arm.animation_data.nla_tracks):  # the importer parks every animation on its own muted track
    arm.animation_data.nla_tracks.remove(t)
meshes = [o for o in scn.objects if o.type == "MESH" and "icosphere" not in o.name.lower()]
for o in scn.objects:
    if o.type == "MESH" and o not in meshes:
        o.hide_render = True

def pick_recall():
    # Main recall clip: skip loops, lead-outs and prop clips, prefer the plain name, else the longest
    acts = [a for a in bpy.data.actions if "recall" in a.name.lower()
            and not re.search(r"loop|leadout|winddown|outro|_out|launcher|drecall|weapon|prop", a.name.lower())]
    if not acts:
        acts = [a for a in bpy.data.actions if re.search(r"dance|taunt|joke", a.name.lower())]
    plain = [a for a in acts if re.fullmatch(r"recall(\.anm|_base|_raw)?", a.name.lower())]
    return max(plain or acts, key=lambda a: a.frame_range[1] - a.frame_range[0])

act = bpy.data.actions[args[3]] if len(args) > 3 else pick_recall()  # optional clip override

def chain(act):
    # Split recalls (lead-in/wind-up, then lead-out where e.g. Jinx rockets away) are joined so the climax is included
    if not re.search(r"lead_?in|wind_?up", act.name.lower()):
        return act
    outs = [a for a in bpy.data.actions if re.fullmatch(r"recall_?(lead_?out|outro)", a.name.lower())]
    if not outs:
        return act
    # Copy every bone's pose frame by frame into one new clip
    poses = []
    for a in (act, outs[0]):
        arm.animation_data.action = a
        for f in range(int(a.frame_range[0]), int(a.frame_range[1]) + 1):
            scn.frame_set(f)
            poses.append({b.name: (b.location.copy(), b.rotation_quaternion.copy(), b.scale.copy()) for b in arm.pose.bones})
    baked = bpy.data.actions.new("joined")
    arm.animation_data.action = baked
    for f, pose in enumerate(poses, 1):
        for b in arm.pose.bones:
            b.location, b.rotation_quaternion, b.scale = pose[b.name]
            for prop in ("location", "rotation_quaternion", "scale"):
                b.keyframe_insert(prop, frame=f)
    baked.name = f"{act.name}+{outs[0].name}"
    return baked

act = chain(act)
a0, a1 = act.frame_range
arm.animation_data.action = act
print("CLIP", act.name, f"{(a1 - a0) / FPS:.2f}s")

# Sample the clip: robust body box (percentiles drop parked props) and motion energy per frame
def sample(f):
    scn.frame_set(int(f))
    dg = bpy.context.evaluated_depsgraph_get()
    vs = []
    for o in meshes:
        e = o.evaluated_get(dg)
        m = e.to_mesh()
        step = max(1, len(m.vertices) // 2500)
        vs += [e.matrix_world @ m.vertices[i].co for i in range(0, len(m.vertices), step)]
        e.to_mesh_clear()
    return vs

frames = list(range(int(a0), int(a1) + 1))
# Body bones: the root branches holding the pelvis (legs) and the head (spine, arms), minus FX attach points,
# so props parked beside the champion don't count toward framing
pb = arm.pose.bones
def branch(name):
    chain = [pb[name]] + list(pb[name].parent_recursive)
    return chain[-2] if len(chain) > 1 else chain[-1]  # the ancestor just below the root bone
body_bones = []
if "Head" in pb and "Pelvis" in pb:
    tops = {branch("Head"), branch("Pelvis")}
    body_bones = [b for b in pb if (b in tops or tops & set(b.parent_recursive)) and "buff" not in b.name.lower()]
boxes, bodies, mids, core, headz, pelz, energy, prev = [], [], [], [], [], [], [], None
# What counts as the champion: mesh within this reach of the head-to-pelvis line (legs, arms, capes, held weapons), not
# things flung or parked farther out (Aphelios' guns, Jhin's prop), even when they hang off the arm bones
reach = None
if "Head" in pb and "Pelvis" in pb:
    reach = 0.5 * 2.7 * (arm.data.bones["Head"].head_local - arm.data.bones["Pelvis"].head_local).length * max(abs(x) for x in arm.matrix_world.to_scale())
def near_torso(v, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (v - a).dot(ab) / max(ab.length_squared, 1e-9)))
    return (v - (a + ab * t)).length <= reach
for f in frames:
    vs = sample(f)
    ax = [sorted(v[i] for v in vs) for i in range(3)]
    lo = Vector([a[int(len(a) * 0.03)] for a in ax]); hi = Vector([a[int(len(a) * 0.97)] for a in ax])
    mids.append(max(a[int(len(a) * 0.9)] - a[int(len(a) * 0.1)] for a in ax[:2]) * 1.25)  # body width without props
    if body_bones:  # bone heads only: the importer gives leaf bones long arbitrary tails
        pts = [arm.matrix_world @ b.head for b in body_bones]
        if reach:  # joints out on flung weapons don't make the body wider
            h, p = arm.matrix_world @ pb["Head"].head, arm.matrix_world @ pb["Pelvis"].head
            pts = [q for q in pts if near_torso(q, h, p)] or pts
        blo, bhi = Vector([min(p[i] for p in pts) for i in range(3)]), Vector([max(p[i] for p in pts) for i in range(3)])
        bodies.append((blo, bhi))
        near = vs
        if reach:
            h, p = arm.matrix_world @ pb["Head"].head, arm.matrix_world @ pb["Pelvis"].head
            near = [v for v in vs if near_torso(v, h, p)] or vs
        ax = [sorted(v[i] for v in near) for i in range(3)]
        boxes.append((Vector([a[int(len(a) * 0.02)] for a in ax]), Vector([a[int(len(a) * 0.98)] for a in ax])))
    else:
        bodies.append((lo, hi))
        boxes.append((lo, hi))
    headz.append((arm.matrix_world @ pb["Head"].head).z if "Head" in pb else 0.0)
    pelz.append((arm.matrix_world @ pb["Pelvis"].head).z if "Pelvis" in pb else -1.0)
    if "Head" in pb and "Pelvis" in pb:  # League rigs share these, so framing keys off the torso, not props
        core.append(arm.matrix_world @ ((pb["Head"].head + pb["Pelvis"].head) / 2))
    else:
        core.append(Vector([(a[int(len(a) * 0.15)] + a[int(len(a) * 0.85)]) / 2 for a in ax]))
    energy.append(0.0 if prev is None else sum((a - b).length for a, b in zip(vs, prev)) / len(vs))
    prev = vs
# Recalls end by teleporting the rig away; cut the clip at the first jump bigger than the body itself
body = boxes[0][1].z - boxes[0][0].z
cut = next((i for i in range(1, len(core)) if (core[i] - core[i - 1]).length > body), len(core))
frames, boxes, bodies, mids, core, headz, pelz, energy = frames[:cut], boxes[:cut], bodies[:cut], mids[:cut], core[:cut], headz[:cut], pelz[:cut], energy[:cut]
smooth = lambda xs, r: [sum(xs[max(0, i - r):i + r + 1]) / len(xs[max(0, i - r):i + r + 1]) for i in range(len(xs))]
en = smooth(energy, 3)
n = len(frames)
# Framing size: standing height in the rest pose, so tails, weapons and props can't shrink the champion in frame
arm.data.pose_position = "REST"
zs = sorted(v.z for v in sample(frames[0]))
arm.data.pose_position = "POSE"
height = zs[int(len(zs) * 0.98)] - zs[int(len(zs) * 0.02)]
# Wide champions and prop-heavy recalls also need room sideways: median width over the clip, not the T-posed rest
widths = sorted(mids)
squat = min(1.0, max(0.6, height / widths[len(widths) // 2] / 2.0))  # tall humanoids ~1, Teemo/Malphite ~0.6
height = max(height, widths[len(widths) // 2])
rest = arm.data.bones
if "Head" in rest and "Pelvis" in rest:
    # Skeleton scale ignores scissors, tails and launchers: a standing body is ~2.7x its head-to-pelvis span
    height = 2.7 * (arm.matrix_world @ rest["Head"].head_local - arm.matrix_world @ rest["Pelvis"].head_local).length
    squat = 1.0
print("HEIGHT", round(height, 2), "SQUAT", round(squat, 2))

# Best part: the most dynamic 1.6s of the recall (later moves weighted up, recalls build to a climax)
D = int(1.6 * FPS)  # the stretch of the recall the intro is built around: its most active 1.6 seconds
lo_i = n // 10
upright = [1.0 if headz[i] > pelz[i] else 0.0 for i in range(n)]  # some recalls flip them over (Dark Cosmic Jhin)
best = max(range(lo_i, max(lo_i + 1, n - D)),
           key=lambda i: sum(en[i:i + D]) * (1 + 0.3 * i / n) * (0.02 + 0.98 * (sum(upright[i:i + D + D // 2]) / len(upright[i:i + D + D // 2])) ** 2))
end = min(n - 1, best + D)
spread = lambda i: (boxes[i][1] - boxes[i][0]).length
# Freeze where the silhouette is most open, upright (head above pelvis) so the face reads
freeze = max(range(best + int(D * 0.5), end + 1), key=lambda i: spread(i) + (10.0 if headz[i] > pelz[i] else 0.0))

# Pacing: real speed into the move, then decelerate through it to a dead stop, then hold the freeze frame
F_IN, F_FREEZE = 40, 125  # output frame where the move starts slowing, and where the pose freezes
e0 = max(0.0, best - (F_IN - 1) * 1.0)
def clip_pos(f):
    # Output frame -> fractional clip frame index
    if f <= F_IN:
        return e0 + (best - e0) * (f - 1) / (F_IN - 1)
    if f <= F_FREEZE:
        u = (f - F_IN) / (F_FREEZE - F_IN)
        return best + (freeze - best) * (1 - (1 - u) ** 2.2)  # speed eases from ~1.2x to zero
    return float(freeze)
print("PACING best", round(best / FPS, 2), "freeze", round(freeze / FPS, 2), "of", round(n / FPS, 2))

arm.animation_data.action = None
strip = arm.animation_data.nla_tracks.new().strips.new("clip", 1, act)
strip.frame_end = TOTAL
strip.use_animated_time = True
for f in range(1, TOTAL + 1):
    strip.strip_time = a0 + clip_pos(f)
    strip.keyframe_insert("strip_time", frame=f)
for kp in strip.fcurves[0].keyframe_points:
    kp.interpolation = "LINEAR"

# Camera keys: one row per key, eased between them, so the camera drifts in slowly, swings round as the move starts, and
# comes to rest as the pose freezes (no push-in on a still frame). Output frames run 1-150 at 30 fps
SHOTS = [
    # frame     angle  height  lens
    (1,         -50,   -0.1,   30),  # angle: degrees round from straight in front of her (minus = her right side)
    (40,        -38,   -0.08,  32),  # height: camera height as a fraction of her height (minus = below her middle)
    (75,         0,    -0.05,  35),  # lens: focal length in mm (bigger = flatter, closer-feeling)
    (F_FREEZE,   14,   -0.03,  38),
    (TOTAL,      14,   -0.03,  38),
]
# How much of the champion fills the frame's height, measured down from the top of their outline: nearly all of them on
# the wide, thighs-up on the freeze. The camera distance is worked out from this every frame, so tall, wide or bowing
# champions all fit
SHOW = [
    # frame     fraction shown
    (1,         0.95),
    (70,        0.85),
    (F_FREEZE,  0.75),
    (TOTAL,     0.75),
]
SIDE = 0.23  # she sits this far right of center (fraction of width), leaving the top-left for the card

def keyed(keys, f):
    # Values at output frame f, eased between keys so the camera never jerks from one speed to another
    if f <= keys[0][0]:
        return list(keys[0][1:])
    for (fa, *a), (fb, *b) in zip(keys, keys[1:]):
        if f <= fb:
            u = (f - fa) / (fb - fa)
            u = u * u * (3 - 2 * u)
            return [x + (y - x) * u for x, y in zip(a, b)]
    return list(keys[-1][1:])

# The outline per clip frame (mesh, so capes and weapons count), smoothed so the camera follows it without shaking;
# read at fractional positions, so slow-mo doesn't step the camera
gauss = [math.exp(-(k / 2.5) ** 2) for k in range(-9, 10)]  # light: heavy smoothing lags fast moves like Jinx's launch
def smoothed(xs):
    out = []
    for i in range(n):
        w = [(gauss[k + 9], xs[i + k]) for k in range(-9, 10) if 0 <= i + k < n]
        out.append(sum((g * x for g, x in w), type(xs[0])() if isinstance(xs[0], Vector) else 0.0) / sum(g for g, _ in w))
    return out
# Framing follows the body's joints near the torso, topped at the head plus room for hair, hats and helmets (a weapon
# raised overhead may run off the top rather than shrink them)
heads = [headz[i] + 0.15 * height if headz[i] > pelz[i] else bodies[i][1].z for i in range(n)]
frame_boxes = [(lo, Vector((hi.x, hi.y, min(hi.z + 0.12 * height, top)))) for (lo, hi), top in zip(bodies, heads)]
mid_xy = smoothed([Vector((c.x, c.y, 0)) for c in core])
tops = smoothed([hi.z for lo, hi in frame_boxes])
spans = smoothed([hi.z - lo.z for lo, hi in frame_boxes])
def at(xs, x):
    i = min(n - 2, int(x)); u = x - i
    return xs[i] + (xs[i + 1] - xs[i]) * u

cam = bpy.data.objects.new("POTG Camera", bpy.data.cameras.new("POTG Camera"))
scn.collection.objects.link(cam)
scn.camera = cam
def framing(f):
    # Aim point and the height of the part that should fill the frame, for output frame f
    x = clip_pos(f)
    keep = keyed(SHOW, f)[0]
    top, span = at(tops, x), at(spans, x)
    c = at(mid_xy, x)
    return Vector((c.x, c.y, top - keep * span / 2)), keep * span
raw = [framing(f) for f in range(1, TOTAL + 1)]
looks = [sum((r[0] for r in raw[max(0, i - 5):i + 6]), Vector()) / len(raw[max(0, i - 5):i + 6]) for i in range(TOTAL)]  # glide
shown = [sum(r[1] for r in raw[max(0, i - 5):i + 6]) / len(raw[max(0, i - 5):i + 6]) for i in range(TOTAL)]
hold = lambda xs: xs[:F_FREEZE] + [xs[F_FREEZE - 1]] * (TOTAL - F_FREEZE)  # still from the freeze on, smoothing included
looks, shown = hold(looks), hold(shown)
def shot(f, back):
    # Camera position, aim point, lens and orbit angle for output frame f, pulled back by the factor back (1 = the shown
    # part exactly fills the frame's height)
    angle, h, lens = keyed(SHOTS, f)
    vfov = 2 * math.atan(18 * 9 / 16 / lens)
    dist = back * shown[f - 1] / (2 * math.tan(vfov / 2))
    a = math.radians(angle)
    look = looks[f - 1]
    pos = look + Vector((math.sin(a) * dist, -math.cos(a) * dist, h * height * back))
    fwd = (look - pos).normalized()
    right = fwd.cross(Vector((0, 0, 1))).normalized()
    shift = -right * SIDE * 2 * (look - pos).length * 18 / lens  # pan left so she lands right of center
    return pos + shift, look + shift, lens, angle

def fits(f, back):
    # The shown part inside the right part of the frame, clear of the card on the left, head never cut
    pos, aim, lens, _ = shot(f, back)
    fwd = (aim - pos).normalized(); right = fwd.cross(Vector((0, 0, 1))).normalized(); up = right.cross(fwd)
    i = min(n - 1, int(round(clip_pos(f))))
    lo, hi = frame_boxes[i]
    bottom = hi.z - keyed(SHOW, f)[0] * (hi.z - lo.z)
    # Side to side the body's joints have to fit (props and swung weapons may run off the edge); up and down, its outline
    pad = 0.1 * height
    blo, bhi = bodies[i]
    for corner in ((x, y, z) for x in (blo.x - pad, bhi.x + pad) for y in (blo.y - pad, bhi.y + pad) for z in (bottom, hi.z)):
        v = Vector(corner) - pos
        depth = v.dot(fwd)
        if depth <= 0:
            return False
        sx, sy = v.dot(right) / (depth * 18 / lens), v.dot(up) / (depth * 18 / lens * 9 / 16)  # -1..1 across the frame
        if not (-0.12 < sx < 0.95 and -1.0 < sy < 0.9):
            return False
    return True

# Pull back on frames where a wide pose (raised arm, cape, weapon) would leave the frame, then smooth so it doesn't pump
backs = []
for f in range(1, TOTAL + 1):
    b = 1.0
    while b < 2.2 and not fits(f, b):  # past this something flies far out (Aphelios' guns); let it crop
        b *= 1.03
    backs.append(b)
backs = [max(backs[max(0, i - 12):i + 13]) for i in range(TOTAL)]
backs = hold([sum(backs[max(0, i - 10):i + 11]) / len(backs[max(0, i - 10):i + 11]) for i in range(TOTAL)])
print("PULLBACK", round(min(backs), 2), "to", round(max(backs), 2))

prev, orbit = None, []
for f in range(1, TOTAL + 1):
    location, aim, lens, angle = shot(f, backs[f - 1])
    orbit.append(angle)
    cam.location = location
    q = (aim - cam.location).to_track_quat("-Z", "Y")
    cam.rotation_euler = q.to_euler("XYZ", prev) if prev else q.to_euler("XYZ")
    prev = cam.rotation_euler.copy()
    cam.data.lens = lens
    cam.keyframe_insert("location", frame=f)
    cam.keyframe_insert("rotation_euler", frame=f)
    cam.data.keyframe_insert("lens", frame=f)

world = bpy.data.worlds.new("w"); scn.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.02, 0.025, 0.05, 1)
for name, rot, power, col in (("key", (50, 0, 35), 4, (1, .95, .9)), ("rim", (60, 0, 200), 6, (.6, .7, 1))):
    l = bpy.data.objects.new(name, bpy.data.lights.new(name, "SUN"))
    l.data.energy, l.data.color = power, col
    l.rotation_euler = [math.radians(r) for r in rot]
    scn.collection.objects.link(l)

r = scn.render
r.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in [e.identifier for e in r.bl_rna.properties["engine"].enum_items] else "BLENDER_EEVEE"  # GPU
r.use_motion_blur = True
scn.eevee.taa_render_samples = 16  # a fraction of the default; the intro plays at half size over the game anyway
r.resolution_x, r.resolution_y, r.resolution_percentage = 1920, 1080, res
# Transparent frames into the out folder, so the app can put the champion over a shot of the map
r.film_transparent = True
if hasattr(r.image_settings, "media_type"):  # Blender 5 splits video out from image formats
    r.image_settings.media_type = "IMAGE"
r.image_settings.file_format, r.image_settings.color_mode = "PNG", "RGBA"
os.makedirs(out, exist_ok=True)
r.filepath = os.path.join(out, "f_")
# The camera's orbit angle per frame, so the background can pan with it
open(os.path.join(out, "orbit.txt"), "w").write("\n".join(f"{orbit[f]:.3f}" for f in range(TOTAL)))
if res > 0:  # res 0 analyses only
    bpy.ops.render.render(animation=True)
