#!/usr/bin/env python3
"""Native/parallel PNG parity, independent mutable images and fallback checks."""
import hashlib
import json
import os
from pathlib import Path
import struct
import unittest
import zlib

import integration as f
from gltf_batching import fixture


def textured(path, case):
    fixture(path, 'static', 16)
    raw=path.read_bytes(); size=struct.unpack_from('<I',raw,12)[0]
    doc=json.loads(raw[20:20+size]); binary=bytearray(raw[28+size:])
    png=path.with_suffix('.png'); f.png(png,(93,168,227,119),32)
    pixels=png.read_bytes()
    if case=='16bit':
        def chunk(kind,data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        pixels=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',32,32,16,2,0,0,0))
        pixels+=chunk(b'IDAT',zlib.compress((b'\0'+struct.pack('>HHH',12345,40000,60000)*32)*32))+chunk(b'IEND',b'')
    doc['images']=[];doc['textures']=[];doc['materials']=[]
    for i in range(16):
        # Deliberately byte-identical images assigned to different mutable roles.
        # Sharing decoded storage would let ORM alpha/component changes leak.
        for role in range(3):
            data=pixels
            if case=='different' and i==7 and role==0:
                f.png(png,(200,40,90,255),32);data=png.read_bytes()
            if case=='corrupt' and i==15 and role==2:data=pixels[:33]
            binary+=b'\0'*(-len(binary)%4)
            view=len(doc['bufferViews'])
            doc['bufferViews'].append({'buffer':0,'byteOffset':len(binary),'byteLength':len(data)})
            binary+=data
            doc['images'].append({'bufferView':view,'mimeType':'image/png'})
            doc['textures'].append({'source':len(doc['images'])-1})
        doc['materials'].append({'doubleSided':True,'pbrMetallicRoughness':{
            'baseColorTexture':{'index':3*i},'metallicRoughnessTexture':{'index':3*i+1}},
            'normalTexture':{'index':3*i+2}})
        doc['meshes'][i]['primitives'][0]['material']=i
    if case=='external':
        png.write_bytes(pixels)
        doc['images'][0]={'uri':png.name,'mimeType':'image/png'}
    if case=='small':
        doc['images']=doc['images'][:3];doc['textures']=doc['textures'][:3]
        for m in doc['materials']:
            m['pbrMetallicRoughness']['baseColorTexture']['index']=0
            m['pbrMetallicRoughness']['metallicRoughnessTexture']['index']=1
            m['normalTexture']['index']=2
    doc['buffers']=[{'byteLength':len(binary)}]
    data=json.dumps(doc,separators=(',',':')).encode();data+=b' '*(-len(data)%4);binary+=b'\0'*(-len(binary)%4)
    path.write_bytes(struct.pack('<III',0x46546c67,2,28+len(data)+len(binary))+struct.pack('<II',len(data),0x4e4f534a)+data+struct.pack('<II',len(binary),0x004e4942)+binary)


@unittest.skipUnless(f.GPU,'Native GPU required')
class GltfImageDecode(unittest.TestCase):
    setUpClass=classmethod(f.PreviewIntegration.setUpClass.__func__)
    tearDownClass=classmethod(f.PreviewIntegration.tearDownClass.__func__)
    setUp=f.PreviewIntegration.setUp

    def restart(self,disabled):
        f.client.rpc({'method':'quit'});f.eventually(lambda:not f.client.running())
        if disabled:os.environ['ASSET_PREVIEW_DISABLE_GLTF_IMAGE_DECODE']='1'
        else:os.environ.pop('ASSET_PREVIEW_DISABLE_GLTF_IMAGE_DECODE',None)
        f.client.start();f.client.rpc({'method':'show'})
        return f.eventually(lambda:(e if e.get('metrics',{}).get('engine') and not e['metrics'].get('loading') and not e['metrics'].get('command_pending') else None)
                            if (e:=f.row('decode')) else None,timeout=30)

    def test_exact_pixels_reload_and_fallback(self):
        for case,unique in [('duplicate',1),('different',2),('external',0),('16bit',0),('small',0)]:
            with self.subTest(case=case):
                f.client.rpc({'method':'hide'})
                path=self.root/'same-path.glb';textured(path,case)
                original=hashlib.sha256(path.read_bytes()).hexdigest()
                f.client.rpc({'method':'add','entry':{'id':'decode','kind':'model','path':str(path),
                    'settings':{'grid':False,'paused':True,'edges':False}}})
                before=self.restart(True)
                first=self.root/(case+'-native.png');f.client.rpc({'method':'capture','path':str(first)})
                after=self.restart(False)
                second=self.root/(case+'-parallel.png');f.client.rpc({'method':'capture','path':str(second)})
                self.assertEqual(before['metrics']['gltf_import']['image_decode'],{})
                decode=after['metrics']['gltf_import']['image_decode']
                self.assertEqual(decode.get('unique_decodes',0),unique)
                if unique:
                    self.assertEqual(decode['images'],48)
                    self.assertLessEqual(decode['workers'],4)
                self.assertEqual(first.read_bytes(),second.read_bytes())
                self.assertEqual(before['metrics']['triangles'],after['metrics']['triangles'])
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),original)
                f.client.rpc({'method':'hide'});self.assertEqual(f.client.rpc({'method':'list'})['active'],0)
                if os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR'):
                    out=Path(os.environ['ASSET_PREVIEW_EVIDENCE_DIR']);out.mkdir(parents=True,exist_ok=True)
                    (out/(case+'.json')).write_text(json.dumps({'before':before['metrics'],'after':after['metrics']},indent=2))
                f.client.rpc({'method':'remove','id':'decode'})

    def test_failed_decode_retains_last_good_scene(self):
        # The native reference crashes on this truncated embedded stream on
        # VTK 9.7. Exercise our bounded decoder and last-good boundary directly.
        for disabled in (False,):
            with self.subTest(native=disabled):
                f.client.rpc({'method':'hide'})
                path=self.root/'replacement.glb';textured(path,'duplicate')
                f.client.rpc({'method':'add','entry':{'id':'decode','kind':'model','path':str(path),
                    'settings':{'grid':False,'paused':True}}})
                before=self.restart(disabled)
                first=self.root/'last-good.png';f.client.rpc({'method':'capture','path':str(first)})
                candidate=self.root/'corrupt.glb';textured(candidate,'corrupt');candidate.replace(path)
                f.client.rpc({'method':'reload','id':'decode'})
                after=f.eventually(lambda:(e if e['status'].startswith('Waiting') and not e.get('metrics',{}).get('loading') else None)
                                   if (e:=f.row('decode')) else None,timeout=30)
                self.assertEqual(before['revisions'],after['revisions'])
                self.assertEqual(before['metrics']['loads'],after['metrics']['loads'])
                second=self.root/'retained.png';f.client.rpc({'method':'capture','path':str(second)})
                self.assertEqual(before['metrics']['camera'],after['metrics']['camera'])
                # The failure message intentionally changes window chrome.
                # Sample scene pixels below the top name/status overlays.
                v=before['viewport']
                points=[(x,y) for y in range(v['y']+120,v['y']+v['height']-20,24)
                        for x in range(v['x']+20,v['x']+v['width']-20,24)]
                pixels=f.png_pixels(first,points)
                self.assertGreater(len(set(pixels)),3)
                self.assertEqual(pixels,f.png_pixels(second,points))
                f.client.rpc({'method':'hide'});f.client.rpc({'method':'remove','id':'decode'})

if __name__=='__main__':unittest.main()
