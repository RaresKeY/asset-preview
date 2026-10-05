#!/usr/bin/env python3
"""Matched event-idle measurements and real-project captures in a headless display."""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
loader=importlib.machinery.SourceFileLoader("preview_tests",str(ROOT/"tests/integration.py"))
spec=importlib.util.spec_from_loader(loader.name,loader); helpers=importlib.util.module_from_spec(spec); loader.exec_module(helpers)
client=helpers.client

def memory(pid):
    values={}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith(("Rss:","Pss:","Private_Clean:","Private_Dirty:")):
            key,value,*_=line.split(); values[key[:-1]+"_kib"]=int(value)
    return values

def main():
    parser=argparse.ArgumentParser(); parser.add_argument("--model",type=Path,required=True)
    parser.add_argument("--albedo",type=Path,required=True); parser.add_argument("--normal",type=Path,required=True)
    parser.add_argument("--orm",type=Path,required=True); parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args(); output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="asset-preview-profile-") as temp:
        fixture=Path(temp); os.environ["ASSET_PREVIEW_RUNTIME_DIR"]=str(fixture/"runtime"); os.environ["ASSET_PREVIEW_STATE_DIR"]=str(fixture/"state")
        client.start(); info=client.rpc({"method":"ping"}); pid=info["pid"]; report={"date":datetime.now(timezone.utc).isoformat(),"platform":info["platform"],"display":os.environ.get("DISPLAY"),"pid":pid,"samples":[],"model":str(args.model.resolve()),"albedo":str(args.albedo.resolve())}
        def sample(name,capture=False):
            time.sleep(.4); before=client.rpc({"method":"list"}); ticks=helpers.ticks(pid); started=time.monotonic()
            time.sleep(1.5); elapsed=time.monotonic()-started; after=client.rpc({"method":"list"})
            row={"name":name,**memory(pid),"idle_seconds":elapsed,"cpu_seconds":(helpers.ticks(pid)-ticks)/os.sysconf("SC_CLK_TCK"),"active":after["active"],"render_delta":{e["id"]:e.get("metrics",{}).get("renders",0)-next((p.get("metrics",{}).get("renders",0) for p in before["entries"] if p["id"]==e["id"]),0) for e in after["entries"] if e["active"]}}
            report["samples"].append(row)
            if capture:
                path=output/(name+".png"); path.unlink(missing_ok=True); client.rpc({"method":"capture","path":str(path)})
            print(json.dumps(row),flush=True)
        try:
            sample("background")
            image=fixture/"image.png"; helpers.png(image,(120,190,220,255),256)
            client.rpc({"method":"add","entry":{"id":"image","label":"Texture · live image","path":str(image)}})
            client.rpc({"method":"show"}); sample("image",True)
            client.rpc({"method":"add","entry":{"id":"model","label":"Sugar cube · textured GLB","path":str(args.model.resolve())}})
            client.rpc({"method":"select","id":"model"}); helpers.eventually(lambda:helpers.row("model").get("metrics",{}).get("engine"),timeout=40)
            report["renderer"]=helpers.row("model")["metrics"]["renderer"]; sample("model",True)
            client.rpc({"method":"add","entry":{"id":"material","label":"Walnut · baked PBR","path":str(args.albedo.resolve()),"kind":"material","maps":{"normal":str(args.normal.resolve()),"orm":str(args.orm.resolve())}}})
            client.rpc({"method":"select","id":"material"}); helpers.eventually(lambda:helpers.row("material").get("metrics",{}).get("engine"),timeout=40); sample("material",True)
            client.rpc({"method":"add","entry":{"id":"albedo","label":"Walnut · albedo","path":str(args.albedo.resolve()),"kind":"image"}})
            client.rpc({"method":"layout","layout":"grid"}); helpers.eventually(lambda:client.rpc({"method":"list"})["active"]==4); sample("grid",True)
            client.rpc({"method":"hide"}); sample("hidden_after_3d")
            (output/"performance.json").write_text(json.dumps(report,indent=2))
        finally:
            client.rpc({"method":"quit"}); helpers.eventually(lambda:not client.running())

if __name__=="__main__": main()
