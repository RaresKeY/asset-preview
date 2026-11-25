#!/usr/bin/env python3
"""Native static batching eligibility, exact pixels and original-input preservation."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import unittest

import integration as f


def fixture(path, case, count=32):
    doc={'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':list(range(count))}],
         'nodes':[],'meshes':[],'materials':[{'pbrMetallicRoughness':{'baseColorFactor':c,'metallicFactor':0,'roughnessFactor':.6},'doubleSided':True}
                                            for c in [[.85,.12,.05,1],[.05,.7,.3,1]]],
         'accessors':[],'bufferViews':[]}
    binary=bytearray()
    def array(values, components, kind):
        while len(binary)%4:binary.append(0)
        offset=len(binary);binary.extend(struct.pack('<'+'f'*len(values),*values))
        view=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(binary)-offset})
        acc={'bufferView':view,'componentType':5126,'count':len(values)//components,'type':kind}
        if kind=='VEC3':
            acc.update(min=[min(values[i::3]) for i in range(3)],max=[max(values[i::3]) for i in range(3)])
        doc['accessors'].append(acc);return len(doc['accessors'])-1
    normal=array([0,0,1]*3,3,'VEC3')
    uv=array([0,0,1,0,.5,1],2,'VEC2')
    for i in range(count):
        x=(i%8-3.5)*1.1;y=(i//8-1.5)*1.1
        pos=array([x-.4,y-.4,0,x+.4,y-.4,0,x,y+.4,0],3,'VEC3')
        prim={'attributes':{'POSITION':pos,'NORMAL':normal,'TEXCOORD_0':uv},'material':i%2,'mode':4}
        if i==0:
            if case=='missing-normal':del prim['attributes']['NORMAL']
            if case=='morph':prim['targets']=[{'POSITION':array([0,0,0]*3,3,'VEC3')}]
            if case=='color':prim['attributes']['COLOR_0']=array([.1,.4,.8,1]*3,4,'VEC4')
        doc['meshes'].append({'primitives':[prim]});doc['nodes'].append({'name':f'original-part-{i}','mesh':i})
    if case=='transformed':doc['nodes'][0]['translation']=[0,0,.2]
    if case in ('blend','mask'):
        doc['materials'][1]['alphaMode']=case.upper()
        doc['materials'][1]['pbrMetallicRoughness']['baseColorFactor'][3]=.7
    if case=='animated':
        times=array([0,1],1,'SCALAR');moves=array([0,0,0,.1,0,0],3,'VEC3')
        doc['animations']=[{'name':'move','samplers':[{'input':times,'output':moves}],
                            'channels':[{'sampler':0,'target':{'node':0,'path':'translation'}}]}]
    if case=='skin':doc['skins']=[{'joints':[0]}]
    doc['buffers']=[{'byteLength':len(binary)}]
    data=json.dumps(doc,separators=(',',':')).encode();data+=b' '*(-len(data)%4);binary+=b'\0'*(-len(binary)%4)
    path.write_bytes(struct.pack('<III',0x46546c67,2,28+len(data)+len(binary))+struct.pack('<II',len(data),0x4e4f534a)+data+struct.pack('<II',len(binary),0x004e4942)+binary)


@unittest.skipUnless(f.GPU, 'Native GPU required')
class GltfBatching(unittest.TestCase):
    setUpClass=classmethod(f.PreviewIntegration.setUpClass.__func__)
    tearDownClass=classmethod(f.PreviewIntegration.tearDownClass.__func__)
    setUp=f.PreviewIntegration.setUp

    def restart(self, disabled):
        f.client.rpc({'method':'quit'});f.eventually(lambda:not f.client.running())
        if disabled:os.environ['ASSET_PREVIEW_DISABLE_GLTF_BATCHING']='1'
        else:os.environ.pop('ASSET_PREVIEW_DISABLE_GLTF_BATCHING',None)
        f.client.start()

    def loaded(self):
        f.client.rpc({'method':'show'})
        return f.eventually(lambda: (e if e.get('metrics',{}).get('engine') and not e['metrics'].get('command_pending') and not e['metrics'].get('loading') else None)
                            if (e:=f.row('batch')) else None, timeout=30)

    def test_static_pixels_fallbacks_and_source_identity(self):
        for case, count, expected in [('static',32,2),('missing-normal',32,3),('color',32,3),
                                      ('transformed',32,3),('blend',32,17),('mask',32,17),
                                      ('morph',32,3),('animated',32,32),('skin',32,32),('small',16,16)]:
            with self.subTest(case=case):
                f.client.rpc({'method':'hide'})
                path=self.root/(case+'.glb');fixture(path,case,count)
                original=hashlib.sha256(path.read_bytes()).hexdigest()
                entry={'id':'batch','kind':'model','path':str(path),'settings':{'grid':False,'paused':True,'edges':True}}
                f.client.rpc({'method':'add','entry':entry})
                self.restart(True);before=self.loaded()
                first=self.root/(case+'-native.png');f.client.rpc({'method':'capture','path':str(first)})
                self.restart(False);after=self.loaded()
                second=self.root/(case+'-batched.png');f.client.rpc({'method':'capture','path':str(second)})
                self.assertEqual(before['metrics']['gltf_import']['render_actors'],count)
                self.assertEqual(after['metrics']['gltf_import']['source_actors'],count)
                self.assertEqual(after['metrics']['gltf_import']['render_actors'],expected)
                self.assertEqual(before['metrics']['triangles'],after['metrics']['triangles'])
                # Non-overlapping fixture geometry must retain exact colors, UVs,
                # normals and coverage against the independent unbatched importer.
                self.assertEqual(first.read_bytes(),second.read_bytes())
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),original)
                f.client.rpc({'method':'hide'});self.assertEqual(f.client.rpc({'method':'list'})['active'],0)
                if os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR'):
                    evidence=Path(os.environ['ASSET_PREVIEW_EVIDENCE_DIR']);evidence.mkdir(parents=True,exist_ok=True)
                    (evidence/(case+'.json')).write_text(json.dumps({'before':before['metrics'],'after':after['metrics']},indent=2))
                f.client.rpc({'method':'remove','id':'batch'})


if __name__=='__main__':unittest.main()
