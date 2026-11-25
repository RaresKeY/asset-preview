#!/usr/bin/env python3
"""Actual source polygons, tessellation edges, counts and replacement safety."""
import time
import unittest
import inspection_integration as harness

POINTS=[(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
        (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]
FACES=[(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]

def cube(path,kind):
    if kind=='obj':
        path.write_text(''.join('v %s %s %s\n'%p for p in POINTS)+
                        ''.join('f '+' '.join(str(i+1) for i in f)+'\n' for f in FACES))
    else:
        path.write_text('ply\nformat ascii 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n'
                        'element face 6\nproperty list uchar int vertex_indices\nend_header\n'+
                        ''.join('%s %s %s\n'%p for p in POINTS)+
                        ''.join('4 '+' '.join(str(i) for i in f)+'\n' for f in FACES))

@unittest.skipUnless(harness.GPU,harness.GPU_REASON)
class SourceTopology(unittest.TestCase):
    def setUp(self):
        self.fixture=harness.InspectionIntegration(methodName='runTest')
        self.fixture._testMethodName=self._testMethodName
        self.addCleanup(self.fixture.doCleanups);self.fixture.setUp()
        self.rpc=self.fixture.rpc;self.row=self.fixture.row

    def settled(self):
        harness.eventually(lambda:self.row().get('metrics',{}).get('engine') and
                           not self.row()['metrics'].get('command_pending'))

    def capture(self,name):
        path=self.fixture.root/name
        self.rpc({'method':'capture','path':str(path)})
        return harness.capture_pixels(path,self.row()['viewport'])

    def patch(self,**settings):
        self.rpc({'method':'settings','id':'model','settings':settings});self.settled()

    def test_source_quad_edges_and_triangle_diagonals(self):
        for kind in ('obj','ply'):
            with self.subTest(format=kind):
                path=self.fixture.root/('cube.'+kind);cube(path,kind)
                self.rpc({'method':'add','entry':{'id':'model','path':str(path),'kind':'model',
                    'settings':{'edges':True,'edge_topology':'source','grid':False,'orthographic':True,'materials':False}}})
                self.rpc({'method':'show'});self.settled()
                self.rpc({'method':'camera_view','id':'model','view':'front'});self.settled()
                state=self.row()['metrics'];topology=state['topology']
                self.assertTrue(state['renderer'])
                self.assertFalse(any(s in state['renderer'].lower() for s in ('unknown','llvmpipe','softpipe','software')))
                self.assertTrue(topology['available'])
                self.assertEqual(topology['quads'],6)
                self.assertEqual(topology['faces'],6)
                self.assertEqual(topology['triangles'],12)
                self.assertEqual(state['triangles'],12)
                source=self.capture(kind+'-source-quads.png')
                self.patch(edge_topology='triangles')
                after=self.row()['metrics'];self.assertEqual(after['loads'],state['loads']+1)
                self.assertEqual(after['topology']['quads'],6,'Tessellation must retain authoritative source counts')
                self.assertEqual(after['topology']['edge_topology'],'triangles')
                triangles=self.capture(kind+'-triangulated.png')
                # Only rendered content can satisfy this comparison. Overlay
                # text and controls were excluded by capture_pixels.
                changed=sum(max(abs(a-b) for a,b in zip(x,y))>20 for x,y in zip(source,triangles))
                self.assertGreater(changed,30,'Explicit triangle mode must show a tessellation diagonal')
                self.patch(edge_topology='source')
                restored=self.capture(kind+'-restored-quads.png')
                self.assertEqual(source,restored,'Restoring source edges must remove the diagonal')
                for key in ('position','focal','up'):
                    for a,b in zip(state['camera'][key],self.row()['metrics']['camera'][key]):self.assertAlmostEqual(a,b,places=9)
                before=self.row()['metrics'];time.sleep(.3)
                self.assertEqual(self.row()['metrics']['renders'],before['renders'])
                self.assertEqual(self.row()['metrics']['display_readbacks'],before['display_readbacks'])
                self.patch(four_view=True)
                self.capture(kind+'-four-view-quads.png')
                self.assertEqual(self.row()['metrics']['topology']['quads'],6)
                if kind=='ply':
                    loads=self.row()['metrics']['loads'];path.write_text('broken export')
                    self.rpc({'method':'reload','id':'model'})
                    harness.eventually(lambda:self.row()['status'].startswith('Waiting'))
                    self.assertEqual(self.row()['metrics']['loads'],loads)
                    self.assertEqual(self.row()['metrics']['topology']['quads'],6)
                    cube(path,kind)
                    self.rpc({'method':'reload','id':'model'});self.settled()
                    self.assertGreater(self.row()['metrics']['loads'],loads)

    def test_triangulated_gltf_and_legacy_four_view(self):
        self.fixture.add_model(settings={'edges':True,'quad_view':True})
        self.rpc({'method':'show'});self.settled()
        self.assertTrue(self.row()['settings']['four_view'])
        self.assertNotIn('quad_view',self.row()['settings'])
        self.assertTrue(self.row()['metrics']['four_view'])
        self.assertEqual(self.row()['metrics']['topology']['quads'],0)
        self.assertEqual(self.row()['metrics']['topology']['status'],'triangulated')
        self.patch(quad_view=False)
        self.assertFalse(self.row()['metrics']['four_view'])
        self.patch(edge_topology='triangles')
        self.assertEqual(self.row()['metrics']['topology']['quads'],0)
        self.capture('gltf-remains-triangles.png')
        with self.assertRaises(RuntimeError):self.patch(edge_topology='guess_quads')

if __name__=='__main__':unittest.main()
