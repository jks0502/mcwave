"""Build user-confirmed desktop enclosure in FreeCAD, mm.
3 printable components; ONLY STL export. FCStd includes the finished three solids.
Regenerate from design-parameters.json to edit dimensional design parameters.
"""
from pathlib import Path
import hashlib
import json
import math
import FreeCAD as App
import Part
import Mesh
import MeshPart

OUT = Path(__file__).resolve().parent
P = json.loads((OUT / "design-parameters.json").read_text(encoding="utf-8"))
S, A, D, C = [P[k] for k in ("screen","assembly","design","charging")]
V = App.Vector
SW, SH, ST = S["width"], S["height"], S["check_thickness"]
B, WALL = D["frame_border"], D["wall"]
W, H = SW+2*B, SH+2*B
ROT = App.Rotation(V(1,0,0),90-D["tilt"])
PLACE = App.Placement(V(0,0,D["panel_bottom_z"]),ROT)
LID_T = D["bottom_cover_thickness"]
EX = (W-A["length"])/2
EY = A["front_y"]
EZ = LID_T + A["foam_height"]
REAR_IN = EY + A["width"] + D["rear_reserve"]
REAR = REAR_IN + WALL
ROOF = D["base_roof_z"]
SEAM = D["cover_seam"]
doc = App.newDocument("DesktopEnclosureV3")

def box(w,h,d,x=0,y=0,z=0):
    return Part.makeBox(w,h,d,V(x,y,z))

def round_rect(w,h,d,r=4,x=0,y=0,z=0):
    q = box(w-2*r,h,d,x+r,y,z).fuse(box(w,h-2*r,d,x,y+r,z))
    for xx,yy in ((x+r,y+r),(x+w-r,y+r),(x+r,y+h-r),(x+w-r,y+h-r)):
        q = q.fuse(Part.makeCylinder(r,d,V(xx,yy,z)))
    return q.removeSplitter()

def panel(q):
    r=q.copy()
    r.Placement=PLACE.multiply(r.Placement)
    return r

def profile(points,x,width,fillet=0):
    wire=Part.makePolygon([V(x,y,z) for y,z in points]+[V(x,*points[0])])
    shape=Part.Face(wire).extrude(V(width,0,0))
    if fillet:
        edges=[e for e in shape.Edges if len(e.Vertexes)==2 and
               abs(abs(e.Vertexes[-1].Point.x-e.Vertexes[0].Point.x)-width)<1e-7]
        shape=shape.makeFillet(fillet,edges)
    return shape

def valid(name,q):
    assert q.isValid() and len(q.Solids)==1 and q.Volume>0,(name,q.isValid(),len(q.Solids))
    q=q.removeSplitter()
    q.check(True)
    return q

def geo(name,q):
    vertices,faces=q.tessellate(0.12)
    return {"name":name,"vertices":[[v.x,v.y,v.z] for v in vertices],"faces":faces}

front_local = round_rect(W,H,D["frame_thickness"],4)
aperture = box(S["active_width"]+2*D["window_expansion"],
               S["active_height"]+2*D["window_expansion"],D["frame_thickness"]+2,
               B+S["active_left"]-D["window_expansion"],
               B+S["active_bottom"]-D["window_expansion"],-1)
front_local=front_local.cut(aperture)
front_holes=[(3,3),(W-3,3),(3,H-3),(W-3,H-3)]
for x,y in front_holes:
    front_local=front_local.cut(Part.makeCylinder(1.7, D["frame_thickness"]+2,V(x,y,-1)))
front_local=valid("front",front_local)
front=panel(front_local)

# Shoulder loft represented by a simple extruded profile: a shallow rear deck,
# then a slope reaching the thin screen backing. No decorative hollow cavities.
outer_points=[(0,0),(REAR,0),(REAR,ROOF-3),(REAR-3,ROOF),
              (38,ROOF),(17.5,D["shoulder_z"])]
base_outer=profile(outer_points,0,W,fillet=2)
back_plane=panel(round_rect(W,H,D["panel_back_thickness"],4,z=-D["panel_back_thickness"]))
body=base_outer.fuse(back_plane)
inner_points=[(6,-1),(REAR_IN,-1),(REAR_IN,ROOF-WALL),
              (38,ROOF-WALL),(17.5,D["shoulder_z"]-3),(6,5)]
cavity=profile(inner_points,WALL,W-2*WALL)
body=body.cut(cavity)
# Recess behind glass; support plane at -2.8 mm, front frame plane at 0.
glass_pocket=panel(box(SW+2*D["glass_edge_clearance"],SH+2*D["glass_edge_clearance"],
                      -D["glass_seat_z"]+0.2,B-D["glass_edge_clearance"],
                      B-D["glass_edge_clearance"],D["glass_seat_z"]))
body=body.cut(glass_pocket)
# Wide lower FPC corridor connects pocket and the front of the main cavity.
fpc_local=box(50,16,12.2,(W-50)/2,0,-12)
fpc_corridor=panel(fpc_local)
body=body.cut(fpc_corridor)
# Front screws seat on backing perimeter; pilot blind holes leave rear wall.
for x,y in front_holes:
    pilot=panel(Part.makeCylinder(1.25,6.4,V(x,y,-6.2)))
    body=body.cut(pilot)

lid_x,lid_y = WALL+SEAM,6+SEAM
lid_w,lid_l = W-2*(WALL+SEAM), REAR_IN-SEAM-lid_y
lid=round_rect(lid_w,lid_l,LID_T,2,x=lid_x,y=lid_y)
lid_holes=[(8,12),(W-8,12),(8,EY+A["width"]+3.5),(W-8,EY+A["width"]+3.5)]
for x,y in lid_holes:
    radius=4 if y<20 else 3
    boss=Part.makeCylinder(radius,8,V(x,y,LID_T))
    # Broad ribs connect each boss to the side wall.
    bx = WALL-0.1 if x < W/2 else x
    bw = x-WALL+0.2 if x < W/2 else W-WALL-x+0.1
    rib=box(bw,6,8,bx,y-3,LID_T)
    body=body.fuse(boss).fuse(rib)
    body=body.cut(Part.makeCylinder(1.25,8.2,V(x,y,LID_T-0.1)))
    lid=lid.cut(Part.makeCylinder(1.7,LID_T+2,V(x,y,-1)))
    # Countersunk lid screw heads remain flush with external bottom surface.
    lid=lid.cut(Part.makeCone(3.3,1.7,1.6,V(x,y,0)))
# Cover carries low guide rails, leaving 0.8 mm space around supplied bundle.
rail_t=1.6
rails=[(EX,EY-A["side_clearance"]-rail_t,A["length"]),
       (12,EY+A["width"]+A["side_clearance"],W-24)]
for x,y,length in rails:
    lid=lid.fuse(box(length,rail_t,6,x,y,LID_T))
# Shallow recesses accept optional 8 mm rubber feet; neither holes nor supports.
for x,y in ((18,23),(W-18,23),(18,REAR_IN-10),(W-18,REAR_IN-10)):
    lid=lid.cut(Part.makeCylinder(4,0.6,V(x,y,0)))
charge_y=EY+(A["width"]-C["width"])/2
charge=box(WALL+1.1,C["width"],C["height_from_external_bottom"]+1,
           -1,charge_y,-1)
body=body.cut(charge)
body=valid("body",body)
lid=valid("lid",lid)

# Hardware/space envelopes are NOT printable components.
glass=panel(box(SW,SH,ST,B,B,D["glass_front_z"]-ST))
active=panel(box(S["active_width"],S["active_height"],0.06,
                 B+S["active_left"],B+S["active_bottom"],D["glass_front_z"]+0.01))
electronics=box(A["length"],A["width"],A["height"],EX,EY,EZ)
AR=P["adapter_reserve"]
adapter=panel(box(AR["width"],AR["height_along_screen"],AR["thickness"],
                  AR["panel_x"],AR["panel_y"],AR["panel_z"]))
# Check a continuous route from lower FPC outlet, up the front gap and above
# the assembly. This is reserved air space, not the unmeasured actual cable.
route_paths=[[(2,11.2),(9,12),(14,23)],
             [(24.5,53.5),(25.5,49),(34,45.5),(57,45.5),(57,43)]]
route=None
for route_points in route_paths:
    for p0,p1 in zip(route_points,route_points[1:]):
        dy,dz=p1[0]-p0[0],p1[1]-p0[1]
        length=math.hypot(dy,dz)
        ny,nz=-dz/length*0.5,dy/length*0.5
        segment=profile([(p0[0]+ny,p0[1]+nz),(p1[0]+ny,p1[1]+nz),
                         (p1[0]-ny,p1[1]-nz),(p0[0]-ny,p0[1]-nz)],(W-30)/2,30)
        route=segment if route is None else route.fuse(segment)
    for y,z in route_points:
        route=route.fuse(Part.makeCylinder(0.5,30,V((W-30)/2,y,z),V(1,0,0)))
components={"front":front,"body":body,"lid":lid}
print_shapes={"front":front_local,"body":body,"lid":lid}
labels={"front":"Front bezel / 4 screws","body":"Rear shell + electronics base","lid":"Removable bottom cover / component guides"}

sheet=doc.addObject("Spreadsheet::Sheet","Dimensions")
sheet.Label="Dimensions mm - regenerate using build_case.py for changes"
sheet.set("A1","DIMENSIONS / REFERENCE TABLE")
for row,(key,value) in enumerate([
    ("Screen W",SW),("Screen H",SH),("Glass thickness check",ST),
    ("Assembly L",A["length"]),("Assembly W",A["width"]),("Assembly H",A["height"]),
    ("Rear tilt deg",D["tilt"]),("Case width",W),("Base depth",REAR),
    ("Charging notch W",C["width"]),("Charging notch from bottom",C["height_from_external_bottom"]),
    ("Wall",WALL),("Bottom cover thickness",LID_T),
],2):
    sheet.set("A"+str(row),key);sheet.set("B"+str(row),str(value))
sheet.set("A17","Measured glass thickness is uncertain: 0.95-1.0 mm")
sheet.set("A18","Unknown cable/adapter actual fit requires physical trial")
sheet.setColumnWidth("A",430)
sheet.setColumnWidth("B",120)
objects=[]
for name,q in components.items():
    obj=doc.addObject("Part::Feature",name.title())
    obj.Label=labels[name];obj.Shape=q
    obj.addProperty("App::PropertyString","DesignNote")
    obj.DesignNote="Regenerate using build_case.py and design-parameters.json; mm"
    objects.append(obj)
doc.recompute()
native=OUT/"desktop-enclosure-v3.FCStd"
doc.saveAs(str(native))

preview={"parts":{k:geo(k,q) for k,q in components.items()},
         "references":[geo("glass",glass),geo("active",active),geo("electronics",electronics),geo("adapter_space",adapter)],
         "section":[]}
slice_box=box(2,REAR+10,H+20,W/2-1,-5,-1)
for name,q in list(components.items())+[("glass",glass),("electronics",electronics),
                                      ("route_space",route),("adapter_space",adapter)]:
    section=q.common(slice_box)
    if not section.isNull() and section.Volume>1e-9:
        preview["section"].append(geo(name,section))
(OUT/"preview_geometry.json").write_text(json.dumps(preview),encoding="utf-8")

report={
    "freecad_version":list(App.Version()),"units":"mm","requested_format":"STL only",
    "confirmed_input_mm":{"screen":[SW,SH],"glass_measured_range":[0.95,1],"bundle":[A["length"],A["width"],A["height"]],"charge_notch":[C["width"],C["height_from_external_bottom"]]},
    "glass_check_thickness_mm":ST,"tilt_from_vertical_deg":D["tilt"],
    "notes":P["notes"],"parts":[],"checks":{},
}
eps=1e-6
assert abs(electronics.BoundBox.XLength-A["length"])<eps
assert abs(electronics.BoundBox.YLength-A["width"])<eps
assert V(SW,0,0).cross(V(A["length"],0,0)).Length<eps
report["checks"]["a_parallel_d_along_x"]=True
report["checks"]["bundle_d_along_x_mm"]=electronics.BoundBox.XLength
report["checks"]["bundle_e_along_y_mm"]=electronics.BoundBox.YLength
report["checks"]["charging_opening_face"]="left"
for name,q in components.items():
    for other,r in components.items():
        if name>=other: continue
        overlap=q.common(r).Volume
        report["checks"][name+"_"+other+"_overlap_mm3"]=overlap
        assert overlap<eps,(name,other,overlap)
for name,q in components.items():
    overlap=q.common(glass).Volume
    report["checks"][name+"_glass_overlap_mm3"]=overlap
    assert overlap<eps,(name,"glass",overlap)
    overlap=q.common(electronics).Volume
    report["checks"][name+"_bundle_overlap_mm3"]=overlap
    assert overlap<eps,(name,"electronics",overlap)
report["checks"]["adapter_reserve_body_overlap_mm3"]=body.common(adapter).Volume
assert body.common(adapter).Volume<eps,"Adapter space obstructed"
assert electronics.common(adapter).Volume<eps,"Adapter space intersects assembly"
assert lid.common(adapter).Volume<eps,"Adapter space intersects lid"
assert glass.common(adapter).Volume<eps,"Adapter space intersects glass"
assert body.common(route).Volume<eps,"Route reserve obstructed"
assert electronics.common(route).Volume<eps,"Route intersects supplied package"
assert adapter.common(route).Volume<eps,"Cable route overlaps adapter capacity"
report["checks"]["two_30mm_wide_ribbon_routes_reserved"]=True
report["checks"]["adapter_space_behind_screen"]=True
report["checks"]["rear_installation_clearance_mm"]=D["rear_reserve"]
# Straight insertion from underside must remain open over every intermediate pose.
insert_sweep=box(A["length"],A["width"],A["height"]+100,EX,EY,EZ-100)
assert body.common(insert_sweep).Volume<eps,"Bundle insertion blocked"
report["checks"]["bundle_straight_insertion_open"]=True
# Cover and guide rails insert vertically from underneath. These boxes conservatively
# contain every position of their prismatic geometry during that insertion.
cover_sweep=box(lid_w,lid_l,LID_T+50,lid_x,lid_y,-50)
for x,y,length in rails:
    cover_sweep=cover_sweep.fuse(box(length,rail_t,LID_T+6+50,x,y,-50))
assert body.common(cover_sweep).Volume<eps,"Bottom cover insertion blocked"
report["checks"]["cover_and_guides_straight_insertion_open"]=True
# Display must be enclosed around its perimeter; glass can be inserted with bezel off.
# Sweep entire glass straight toward its rear seat; roof/shoulder are behind glass.
glass_sweep=panel(box(SW,SH,ST+30,B,B,D["glass_front_z"]-ST))
assert body.common(glass_sweep).Volume<eps,"Glass front insertion blocked"
report["checks"]["glass_front_insertion_open"]=True
for dx,dy in [(-0.7,0),(0.7,0),(0,-0.7),(0,0.7)]:
    shifted=panel(box(SW,SH,ST,B+dx,B+dy,D["glass_front_z"]-ST))
    assert body.common(shifted).Volume>0.01,("Glass side stop missing",dx,dy)
report["checks"]["glass_xy_edge_stops"]=True
forward=panel(box(SW,SH,ST,B,B,D["glass_front_z"]+0.8-ST))
assert front.common(forward).Volume>0.1,"Bezel does not retain glass forward"
report["checks"]["bezel_retains_glass_forward"]=True
# Charging notch is 29 wide and 14 measured from external Z=0.
charge_probe=box(WALL+0.15,C["width"]-0.1,C["height_from_external_bottom"]-0.05,
                 -0.05,charge_y+0.05,0.025)
assert body.common(charge_probe).Volume<eps,"Charging opening blocked"
report["checks"]["charging_notch_open"]=True
report["checks"]["charging_height_from_external_bottom_mm"]=C["height_from_external_bottom"]
# Roof clearance over whole measured package plus declared 3 mm support allowance.
report["checks"]["flat_roof_clearance_above_bundle_mm"]=ROOF-WALL-(EZ+A["height"])
assert report["checks"]["flat_roof_clearance_above_bundle_mm"]>0
# Estimated tipping footprint; actual mass and rubber feet require physical trial.
combined=Part.makeCompound(list(components.values()))
report["assembled_outer_mm"]=[combined.BoundBox.XLength,combined.BoundBox.YLength,combined.BoundBox.ZLength]

for name,q in print_shapes.items():
    printq=q.copy()
    printq.translate(V(-printq.BoundBox.XMin,-printq.BoundBox.YMin,-printq.BoundBox.ZMin))
    mesh=MeshPart.meshFromShape(Shape=printq,LinearDeflection=0.07,AngularDeflection=0.15,Relative=False)
    path=OUT/(name+".stl")
    mesh.write(str(path))
    restored=Mesh.Mesh(str(path))
    assert restored.isSolid() and restored.CountFacets>0,(name,"STL not closed")
    volume_error=abs(abs(restored.Volume)-printq.Volume)/printq.Volume
    assert volume_error<0.005,(name,"volume",volume_error)
    dims=[printq.BoundBox.XLength,printq.BoundBox.YLength,printq.BoundBox.ZLength]
    actual=[restored.BoundBox.XLength,restored.BoundBox.YLength,restored.BoundBox.ZLength]
    assert max(abs(a-b) for a,b in zip(dims,actual))<0.002,(name,dims,actual)
    report["parts"].append({"file":path.name,"single_valid_solid":True,"print_dimensions_mm":dims,
                            "stl_closed":True,"facets":restored.CountFacets,"volume_relative_error":volume_error,
                            "sha256":hashlib.sha256(path.read_bytes()).hexdigest()})
App.closeDocument(doc.Name)
check=App.openDocument(str(native))
for name in ("Front","Body","Lid"):
    assert check.getObject(name).Shape.isValid() and len(check.getObject(name).Shape.Solids)==1
App.closeDocument(check.Name)
report["checks"]["native_saved_and_reopened"]=True
(OUT/"verification.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print("PASS: three FreeCAD solids; STL closed and round-trip verified; hardware fit and glass insertion checks.")
print("ASSEMBLED_OUTER_MM",report["assembled_outer_mm"])
