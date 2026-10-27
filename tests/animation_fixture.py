"""Deterministic animated GLBs, including a two-joint skinned mesh."""
import json
from pathlib import Path
import struct


def animation_bytes(kind="rig",duration=2.0):
    document={"asset":{"version":"2.0","generator":"Asset Preview animation fixture"},
        "scene":0,"scenes":[{"nodes":[0,2]}],
        "nodes":[{"name":"Root joint","children":[1]},
                 {"name":"Tip joint","translation":[0,1,0]},
                 {"name":"Animated panel","mesh":0}],
        "meshes":[],"accessors":[],"bufferViews":[],
        "materials":[{"doubleSided":True,"pbrMetallicRoughness":{
            "baseColorFactor":[.15,.8,.45,1],"metallicFactor":0,"roughnessFactor":.7}}]}
    binary=bytearray()
    def data(values,component_type,kind):
        while len(binary)%4:binary.append(0)
        offset=len(binary)
        components={"SCALAR":1,"VEC3":3,"VEC4":4,"MAT4":16}[kind]
        flat=values if components==1 else [n for value in values for n in value]
        binary.extend(struct.pack("<"+({5126:"f",5123:"H"}[component_type])*len(flat),*flat))
        view=len(document["bufferViews"])
        document["bufferViews"].append({"buffer":0,"byteOffset":offset,"byteLength":len(binary)-offset})
        accessor={"bufferView":view,"componentType":component_type,"count":len(values),"type":kind}
        if kind=="VEC3":
            accessor["min"]=[min(v[i] for v in values) for i in range(3)]
            accessor["max"]=[max(v[i] for v in values) for i in range(3)]
        if kind=="SCALAR":accessor.update(min=[min(values)],max=[max(values)])
        index=len(document["accessors"]);document["accessors"].append(accessor);return index
    positions=[(-.4,0,0),(.4,0,0),(-.4,2,0),(.4,2,0)]
    primitive={"attributes":{"POSITION":data(positions,5126,"VEC3"),
        "NORMAL":data([(0,0,1)]*4,5126,"VEC3")},"indices":data([0,1,2,2,1,3],5123,"SCALAR"),"material":0}
    document["meshes"]=[{"primitives":[primitive]}]
    times=data([0,duration],5126,"SCALAR")
    if kind=="rig":
        primitive["attributes"]["JOINTS_0"]=data([(0,0,0,0)]*2+[(1,0,0,0)]*2,5123,"VEC4")
        primitive["attributes"]["WEIGHTS_0"]=data([(1,0,0,0)]*4,5126,"VEC4")
        identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
        inverse=identity.copy();inverse[13]=-1
        document["skins"]=[{"joints":[0,1],"skeleton":0,"inverseBindMatrices":data([identity,inverse],5126,"MAT4")}]
        document["nodes"][2]["skin"]=0
        target={"node":1,"path":"rotation"}
        output=data([(0,0,0,1),(0,0,.70710678,.70710678)],5126,"VEC4")
    elif kind=="morph":
        document["meshes"][0]["weights"]=[0]
        primitive["targets"]=[{"POSITION":data([(0,0,0),(0,0,0),(1.5,0,0),(1.5,0,0)],5126,"VEC3")}]
        target={"node":2,"path":"weights"};output=data([0,1],5126,"SCALAR")
    elif kind=="node":
        target={"node":2,"path":"rotation"}
        output=data([(0,0,0,1),(0,0,.70710678,.70710678)],5126,"VEC4")
    else:raise ValueError(kind)
    document["animations"]=[
        {"name":kind+" deformation","samplers":[{"input":times,"output":output,"interpolation":"LINEAR"}],
         "channels":[{"sampler":0,"target":target}]},
        {"name":"Slide","samplers":[{"input":times,"output":data([(0,0,0),(1.5,0,0)],5126,"VEC3"),"interpolation":"LINEAR"}],
         "channels":[{"sampler":0,"target":{"node":0 if kind=="rig" else 2,"path":"translation"}}]}]
    document["buffers"]=[{"byteLength":len(binary)}]
    encoded=json.dumps(document,separators=(",",":")).encode();encoded+=b" "*(-len(encoded)%4)
    binary+=b"\0"*(-len(binary)%4)
    body=struct.pack("<I4s",len(encoded),b"JSON")+encoded+struct.pack("<I4s",len(binary),b"BIN\0")+binary
    return struct.pack("<4sII",b"glTF",2,12+len(body))+body


def write_animation(path:Path,kind="rig",duration=2.0):
    candidate=path.with_suffix(".candidate");candidate.write_bytes(animation_bytes(kind,duration));candidate.replace(path)
