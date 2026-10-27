#!/usr/bin/env python3
"""Native POST/O-Voxel imports through a private Asset Preview service."""
import hashlib
import json
import os
from pathlib import Path
import struct
import time
import unittest

import inspection_integration as harness
from inspection_integration import eventually, capture_pixels


def vxz_fixture():
    # Explicit octree golden: origin octant of a 4^3 chunk, all eight leaves.
    colors = bytes(value for i in range(8) for value in (240, 20+i*15, 10))
    body = bytes([1, 255]) + bytes(24) + bytes([7, 0, 0, 0, 0, 0, 0, 0]) + colors
    header = dict(num_voxel=8, chunk_size=4, filter="none", compression="none", compression_level=0,
                  attr_interleave="as_is", attr=[["dual_vertices",3],["intersected",1],["base_color",3]],
                  chunks=[dict(idx=[0,0,0], ptr=[0,len(body)], svo=[0,2], dual_vertices=[2,24],
                               intersected=[26,8], base_color=[34,24])])
    text = json.dumps(header).encode()
    return b"VXZ\0" + struct.pack(">I",8+len(text)) + text + body


def float_fixture():
    coords = [(3,4,5),(3,4,6),(3,5,6),(3,5,5)]
    fields = [(0,0,0,1 if i==0 else -1,-1,-1,0) for i in range(4)]
    return b"OVOXEL01" + struct.pack("<4I",16,4,7,0) + b"".join(struct.pack("<3i",*p) for p in coords) + b"".join(struct.pack("<7f",*p) for p in fields)


class OVoxelIntegration(unittest.TestCase):
    def setUp(self):
        self.fixture = harness.InspectionIntegration(methodName="runTest")
        self.fixture._testMethodName = self._testMethodName
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root,self.rpc,self.row = self.fixture.root,self.fixture.rpc,self.fixture.row

    def add(self,path,key="model",**settings):
        return self.rpc({"method":"add","entry":{"id":key,"kind":"model","path":str(path),
                        "settings":{"grid":False,"axes":False,**settings}}})

    def ready(self,key="model"):
        try:
            return eventually(lambda:self.row(key) if self.row(key).get("metrics",{}).get("loads",0) else None,
                              timeout=40,description=f"native import of {key}")
        except AssertionError:
            state=self.rpc({"method":"list"})
            (self.root/"failed-state.json").write_text(json.dumps(state,indent=2))
            raise AssertionError(f"Native import failed: {self.row(key)}")

    def test_setting_validation_and_private_mode(self):
        path=self.root/"test.vxz";path.write_bytes(vxz_fixture());self.add(path)
        for mode in ("voxels","cells","vertices","surface"):
            self.rpc({"method":"settings","id":"model","settings":{"ovoxel_mode":mode}})
            self.assertEqual(self.row()["settings"]["ovoxel_mode"],mode)
        self.assertNotIn("ovoxel_mode",self.rpc({"method":"list"})["preferences"].get("3d",{}))
        with self.assertRaisesRegex(RuntimeError,"O-Voxel mode"):
            self.rpc({"method":"settings","id":"model","settings":{"ovoxel_mode":"invalid"}})

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_native_imports_modes_and_lifecycle(self):
        paths=[]
        for name,data in (("colored.vxz",vxz_fixture()),("raw.ovoxel",float_fixture())):
            path=self.root/name;path.write_bytes(data);paths.append(path)
        # Existing POST support remains available in the same optional module.
        post=self.root/"mesh.post"
        post.write_bytes(struct.pack("<4i",4,4,0,16)+struct.pack("<12f",0,0,0,1,0,0,0,1,0,0,0,1)+struct.pack("<12i",0,2,1,0,1,3,1,2,3,2,0,3))
        self.add(paths[0]);self.rpc({"method":"select","id":"model"});self.rpc({"method":"show"});first=self.ready()
        self.assertIn("NVIDIA",first["metrics"]["renderer"])
        self.assertNotIn("llvmpipe",first["metrics"]["renderer"].lower())
        for mode in ("surface","voxels","cells","vertices"):
            if mode!="surface":
                old=self.row()["metrics"]["loads"]
                self.rpc({"method":"settings","id":"model","settings":{"ovoxel_mode":mode}})
                eventually(lambda:self.row().get("metrics",{}).get("loads",0)>old,timeout=30)
            time.sleep(.15)
            capture=self.root/f"vxz-{mode}.png";self.rpc({"method":"capture","path":str(capture)})
            pixels=capture_pixels(capture,self.row()["viewport"])
            self.assertGreater(sum(r>g*1.3 and r>b*1.3 and r>70 for r,g,b in pixels),10 if mode=="surface" else 0,"Missing generated voxel colors")
        # Mode choices remain per registration even when both views are active.
        self.add(paths[1],"field",ovoxel_mode="surface");self.rpc({"method":"select","id":"field"});self.ready("field")
        self.rpc({"method":"capture","path":str(self.root/"float-surface.png")})
        self.add(post,"post",up_axis="z");self.rpc({"method":"select","id":"post"});self.ready("post")
        self.rpc({"method":"select","id":"model"});self.ready()
        self.assertEqual(self.row()["settings"]["ovoxel_mode"],"vertices")
        # A corrupt watched replacement retains the previous successful engine.
        before=self.row()["metrics"]["loads"];paths[0].write_bytes(vxz_fixture()[:-1])
        time.sleep(.6)
        self.assertEqual(self.row()["metrics"]["loads"],before)
        self.assertTrue(self.row()["metrics"]["engine"])
        paths[0].write_bytes(vxz_fixture())
        self.rpc({"method":"reload","id":"model"});eventually(lambda:self.row()["metrics"]["loads"]>before,timeout=30)
        # Sources are read-only to the reader, and hidden views release engines.
        self.assertEqual(hashlib.sha256(paths[1].read_bytes()).digest(),hashlib.sha256(float_fixture()).digest())
        self.rpc({"method":"hide"});self.assertNotIn("metrics",self.row())

    @unittest.skipUnless(harness.GPU and os.environ.get("ASSET_PREVIEW_OVOXEL_MODEL"),"Optional real saved decoder field")
    def test_real_saved_field(self):
        path=Path(os.environ["ASSET_PREVIEW_OVOXEL_MODEL"]);before=hashlib.sha256(path.read_bytes()).hexdigest()
        self.add(path);self.rpc({"method":"select","id":"model"});self.rpc({"method":"show"});self.ready()
        for mode in ("surface","voxels","vertices","cells"):
            self.rpc({"method":"settings","id":"model","settings":{"ovoxel_mode":mode}})
            time.sleep(.2);self.ready();self.rpc({"method":"capture","path":str(self.root/f"real-{mode}.png")})
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),before)
        self.rpc({"method":"hide"});self.assertNotIn("metrics",self.row())

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_marker_area_scales_with_orthographic_zoom(self):
        path=self.root/"zoom.ovoxel"
        coords=[(3,8,8),(9,8,8)]
        path.write_bytes(b"OVOXEL01"+struct.pack("<4I",16,2,7,0)+b"".join(struct.pack("<3i",*p) for p in coords)+struct.pack("<14f",*([0,0,0,-1,-1,-1,0]*2)))
        counts=[]
        for span in (1.,.5):
            self.rpc({"method":"add","entry":{"id":"model","kind":"model","path":str(path),
                "camera":{"position":[-.09375,.03125,2.03125],"focal":[-.09375,.03125,.03125],"up":[0,1,0],"angle":30,"parallel_span":span},
                "settings":{"ovoxel_mode":"voxels","orthographic":True,"grid":False,"axes":False,"background":"dark"}}})
            self.rpc({"method":"select","id":"model"});self.rpc({"method":"show"});self.ready()
            time.sleep(.15)
            capture=self.root/f"zoom-{span}.png";self.rpc({"method":"capture","path":str(capture)})
            viewport=self.row()["viewport"]
            center={"x":viewport["x"]+viewport["width"]//10,"y":viewport["y"]+viewport["height"]//4,
                    "width":viewport["width"]*8//10,"height":viewport["height"]//2}
            counts.append(sum(r>65 and g>65 and b>70 for r,g,b in capture_pixels(capture,center)))
        self.assertGreater(counts[0],10,"World-size markers were not visible")
        self.assertGreater(counts[1]/counts[0],2.8,"Markers retained screen size when zoom doubled")
        self.assertLess(counts[1]/counts[0],5.2,"Marker footprint grew beyond its world-space size")


if __name__=="__main__":unittest.main()
