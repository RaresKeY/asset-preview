#!/usr/bin/env python3
"""Isolated, hardware-backed UI scenes; never touches the desktop preview session."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
loader=importlib.machinery.SourceFileLoader("preview_ui_helpers",str(ROOT/"tests/integration.py"))
spec=importlib.util.spec_from_loader(loader.name,loader); helpers=importlib.util.module_from_spec(spec); loader.exec_module(helpers)
client=helpers.client

def main():
    parser=argparse.ArgumentParser(); parser.add_argument("--phase",choices=["before","after"],required=True)
    parser.add_argument("--output",type=Path,required=True); args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    atlas=ROOT.parent/"material-atlas"
    files={"model":atlas/"assets/models/sugar_cube/sugar_cube_textured.glb",
           "albedo":atlas/"assets/materials/smoked_walnut_albedo.png",
           "normal":atlas/"assets/materials/smoked_walnut_normal.png",
           "orm":atlas/"assets/materials/smoked_walnut_orm.png"}
    report={"phase":args.phase,"source_sha256":{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in files.items()},"captures":[]}
    with tempfile.TemporaryDirectory(prefix="asset-preview-ui-") as folder:
        fixture=Path(folder); os.environ["ASSET_PREVIEW_RUNTIME_DIR"]=str(fixture/"runtime"); os.environ["ASSET_PREVIEW_STATE_DIR"]=str(fixture/"state")
        client.start(); input=None
        try:
            client.rpc({"method":"add","entry":{"id":"model","path":str(files["model"]),"label":"Sugar cube · textured GLB"}})
            client.rpc({"method":"add","entry":{"id":"material","kind":"material","path":str(files["albedo"]),"maps":{"normal":str(files["normal"]),"orm":str(files["orm"])},"label":"Smoked walnut · baked material"}})
            for i in range(16):
                path=fixture/f"image{i}.png"; helpers.png(path,(40+i*10,130,210-i*8,255),64)
                client.rpc({"method":"add","entry":{"id":f"image{i}","path":str(path),"label":("Very long asset name to check truncation and control reachability" if i==0 else f"Texture {i+1}")}})
            client.rpc({"method":"layout","layout":"grid"}); client.rpc({"method":"show"})
            helpers.eventually(lambda:helpers.row("model").get("metrics",{}).get("engine"),timeout=40)
            input=helpers.XInput(); input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
            def capture(name,width=1000,height=720):
                input.x.XResizeWindow(input.display,input.window,width,height); input.x.XFlush(input.display); time.sleep(.35)
                path=output/f"{args.phase}-{name}.png"; path.unlink(missing_ok=True)
                client.rpc({"method":"capture","path":str(path)})
                report["captures"].append({"file":path.name,"state":client.rpc({"method":"list"})})
            capture("normal-2x2"); capture("small-2x2",700,500)
            if args.phase=="after":
                client.rpc({"method":"layout","layout":"grid","grid_size":3,"compact":True}); capture("compact-3x3",1000,720)
                client.rpc({"method":"layout","layout":"grid","grid_size":4}); capture("compact-4x4",1600,1000)
                client.rpc({"method":"layout","layout":"single","compact":False}); capture("studio",1000,720)
                client.rpc({"method":"settings","id":"model","settings":{"textures":False}}); capture("no-textures")
                client.rpc({"method":"settings","id":"model","settings":{"materials":False}}); capture("clay")
                client.rpc({"method":"settings","id":"model","settings":{"materials":True,"textures":True,"lighting":"lightkit"}}); capture("light-kit")
            report["renderer"]=helpers.row("model")["metrics"]["renderer"]
            assert not any(token in report["renderer"].lower() for token in ("llvmpipe","softpipe","software","unknown"))
            if args.phase=="after":
                client.rpc({"method":"layout","layout":"grid","grid_size":4,"compact":True})
                client.rpc({"method":"add","entry":{"id":"image0","path":str(fixture/"pending.png"),"label":"Texture baking · waiting for output"}})
                capture("compact-waiting",1600,1000)
                client.rpc({"method":"hide"})
                for entry in client.rpc({"method":"list"})["entries"]:
                    client.rpc({"method":"remove","id":entry["id"]})
                client.rpc({"method":"show"}); capture("empty",700,500)
            (output/f"{args.phase}.json").write_text(json.dumps(report,indent=2))
        finally:
            if input: input.close()
            if client.running(): client.rpc({"method":"quit"}); helpers.eventually(lambda:not client.running())

if __name__=="__main__": main()
