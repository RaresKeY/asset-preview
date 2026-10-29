#!/usr/bin/env python3
"""Persistent project publication, overview and real interactive pane acceptance."""
import ctypes
import json
import os
import subprocess
import struct
import sys
import time
import unittest
from pathlib import Path
import inspection_integration as harness
from overview_integration import OverviewIntegration
from integration import XInput, audio, video, ticks

class ProjectsIntegration(OverviewIntegration):
    # Reuse harness helpers without inheriting its test suite (filtered below).
    def state(self): return self.rpc({'method':'list'})
    def projects(self): return {p['id']:p for p in self.state()['projects']}
    def send(self,key,project='alpha',current=True):
        path=self.root/(key+'.png')
        from integration import png
        png(path,(50+len(key)*9,130,210,255),64)
        self.rpc({'method':'add','entry':{'id':key,'path':str(path),'project':project},'make_current':current})
        return path

    def test_project_publication_persistence_and_rollback(self):
        self.rpc({'method':'project','id':'alpha','label':'Alpha'})
        self.assertEqual(self.projects()['alpha']['current'],'')
        a=self.send('a'); self.send('support',current=False); self.send('other','beta')
        self.assertEqual(self.projects()['alpha']['current'],'a')
        original=self.state()
        self.rpc({'method':'add','entry':{'id':'support','path':str(self.root/'support.png')}})
        self.assertEqual(self.projects()['alpha']['current'],'support','reconnect inherits project and promotes')
        for key in ('selected','layout','window_visible'): self.assertEqual(self.state()[key],original[key])
        self.rpc({'method':'project','id':'alpha','current':'a'})
        for request in [
            {'method':'project','id':'alpha','current':'other'},
            {'method':'project','id':'../bad'},
            {'method':'project','id':'alpha','action':'remove'},
            {'method':'layout','project_filter':'missing'},
            {'method':'layout','project_grid_size':1},
            {'method':'layout','group_projects':'yes'},
            {'method':'add','entry':{'id':'bad','path':str(a),'project':7}},
        ]:
            with self.assertRaises(RuntimeError): self.rpc(request)
        self.rpc({'method':'layout','layout':'projects','project_grid_size':3,'project_filter':'alpha','group_projects':False})
        registry=self.fixture.state_dir/'previews.json'; backup=registry.with_suffix('.backup')
        before=self.state(); registry.rename(backup); registry.mkdir()
        try:
            for request in [
                {'method':'add','entry':{'id':'new','path':str(a),'project':'new-project'}},
                {'method':'add','entry':{'id':'support','path':str(self.root/'support.png')}},
                {'method':'project','id':'alpha','current':'support','label':'Wrong'},
                {'method':'layout','project_grid_size':4,'group_projects':True,'project_filter':'beta'},
            ]:
                with self.assertRaises(RuntimeError): self.rpc(request)
                after=self.state()
                for key in ('projects','entries','layout','project_grid_size','group_projects','project_filter'): self.assertEqual(before[key],after[key],key)
        finally: registry.rmdir(); backup.rename(registry)
        self.restart()
        for key in ('projects','layout','project_grid_size','group_projects','project_filter'): self.assertEqual(before[key],self.state()[key],key)
        self.rpc({'method':'remove','id':'a'}); self.assertEqual(self.projects()['alpha']['current'],'support')
        self.rpc({'method':'add','entry':{'id':'a','path':str(a),'project':'alpha'},'make_current':False})
        self.assertEqual(self.projects()['alpha']['current'],'support','no-current re-add must retain the removal fallback')
        self.restart(); self.assertEqual(self.projects()['alpha']['current'],'support')
        self.rpc({'method':'close_all'}); self.assertEqual(self.projects()['alpha']['current'],'')
        self.rpc({'method':'project','id':'alpha','action':'remove'}); self.assertEqual(self.state()['project_filter'],'')
        self.rpc({'method':'reopen'}); self.assertTrue(self.state()['entries'])

    def test_project_grid_visibility_switching_and_overview(self):
        self.send('alpha-a'); self.send('beta-a','beta'); self.send('alpha-b'); self.send('legacy','')
        self.rpc({'method':'project','id':'empty','label':'Waiting for first output'})
        self.rpc({'method':'layout','layout':'projects'}); self.rpc({'method':'show'})
        state=harness.eventually(lambda:self.state() if self.state()['active']==3 else None)
        self.assertEqual({r['id'] for r in state['entries'] if r['active']},{'alpha-b','beta-a','legacy'})
        self.assertEqual(len(state['presentation']['projects']['panes']),4)
        for layout,active in [('single',1),('grid',4),('projects',3)]:
            self.rpc({'method':'layout','layout':layout})
            harness.eventually(lambda:self.state()['active']==active)
        self.rpc({'method':'project','id':'alpha','current':'alpha-a'})
        harness.eventually(lambda:self.row('alpha-a')['active'] and not self.row('alpha-b')['active'])
        self.rpc({'method':'layout','project_filter':'alpha'})
        harness.eventually(lambda:self.state()['active']==1)
        self.rpc({'method':'layout','layout':'overview','project_filter':'','group_projects':True})
        state=self.state(); self.assertEqual(state['active'],0)
        self.assertEqual([g['id'] for g in state['presentation']['overview']['groups']],['alpha','beta',''])
        self.rpc({'method':'layout','group_projects':False})
        state=self.state(); self.assertFalse(state['presentation']['overview']['groups'])
        self.rpc({'method':'layout','project_filter':'alpha'})
        self.assertEqual(self.state()['presentation']['overview']['items'],2)
        self.rpc({'method':'layout','layout':'projects','project_filter':''}); self.rpc({'method':'hide'})
        self.assertEqual(self.state()['active'],0)

    def test_select_project_current_is_atomic_and_handles_an_already_selected_member(self):
        self.send('alpha-a'); self.send('alpha-b',current=False); self.send('beta','beta'); self.send('legacy','')
        self.rpc({'method':'layout','layout':'projects'})
        self.rpc({'method':'select','id':'alpha-b','make_current':True})
        state=self.state()
        self.assertEqual(state['selected'],'alpha-b'); self.assertEqual(self.projects()['alpha']['current'],'alpha-b')
        self.rpc({'method':'project','id':'alpha','current':'alpha-a'})
        self.rpc({'method':'select','id':'alpha-b','make_current':True})
        self.assertEqual(self.projects()['alpha']['current'],'alpha-b','already-selected members still promote')
        before=self.state()
        for request in ({'method':'select','id':'alpha-a','make_current':1},
                        {'method':'select','id':'legacy','make_current':True}):
            with self.assertRaises(RuntimeError): self.rpc(request)
            for key in ('selected','projects','layout'): self.assertEqual(before[key],self.state()[key])
        registry=self.fixture.state_dir/'previews.json'; backup=registry.with_suffix('.backup')
        registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): self.rpc({'method':'select','id':'alpha-a','make_current':True})
            for key in ('selected','projects','layout'): self.assertEqual(before[key],self.state()[key])
        finally: registry.rmdir(); backup.rename(registry)
        self.restart()
        for key in ('selected','projects','layout'): self.assertEqual(before[key],self.state()[key])

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_hovered_project_arrows_switch_members_and_preserve_native_focus(self):
        self.send('alpha-image'); self.send('beta-image','beta'); self.send('beta-alternative','beta',False)
        movie=self.root/'motion.mp4'; video(movie)
        self.rpc({'method':'add','entry':{'id':'alpha-video','path':str(movie),'project':'alpha',
            'settings':{'paused':True,'muted':True}},'make_current':False})
        self.rpc({'method':'add','entry':{'id':'alpha-model','path':str(self.fixture.model),'project':'alpha'},'make_current':False})
        self.rpc({'method':'project','id':'empty'}); self.send('legacy','')
        self.rpc({'method':'layout','layout':'projects','project_grid_size':2}); self.rpc({'method':'show'})
        self.loaded('alpha-image'); self.loaded('beta-image'); self.loaded('legacy')
        input=XInput(); self.addCleanup(input.close)
        def hover(project,header=False):
            pane=next(p for p in self.state()['presentation']['projects']['panes'] if p['project']==project)
            if header or not pane['current']:
                x,y=pane['x']+30,pane['y']+(12 if header else 100)
            else:
                r=self.row(pane['current'])['viewport']; x,y=r['x']+r['width']//2,r['y']+r['height']//2
            input.motion(x,y); input.x.XFlush(input.display); time.sleep(.15)
        def arrow(key,project,target,alt=False):
            if alt: input.chord('Alt_L',key)
            else: input.press(key)
            harness.eventually(lambda:self.projects()[project]['current']==target and self.state()['selected']==target)
            self.loaded(target)
            self.assertEqual(self.state()['layout'],'projects')
        hover('alpha'); arrow('Left','alpha','alpha-image')
        arrow('Right','alpha','alpha-video')
        # A stationary pointer must keep working after a native video/model is destroyed.
        arrow('Right','alpha','alpha-model')
        renderer=self.row('alpha-model')['metrics']['renderer']
        self.assertFalse(any(token in renderer.lower() for token in ('llvmpipe','softpipe','software','unknown')),renderer)
        arrow('Right','alpha','alpha-model'); arrow('Left','alpha','alpha-video')
        arrow('Left','alpha','alpha-image',alt=True)
        self.assertEqual(self.projects()['beta']['current'],'beta-image')
        hover('beta',header=True)
        # Observe actual registry replacement: pane choice and selection must
        # never be exposed as two separately committed states.
        libc=ctypes.CDLL(None,use_errno=True)
        libc.inotify_add_watch.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint32]
        notify=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
        self.assertGreaterEqual(notify,0)
        try:
            self.assertGreaterEqual(libc.inotify_add_watch(notify,os.fsencode(self.fixture.state_dir),0x80),0)
            arrow('Right','beta','beta-alternative')
            events=os.read(notify,65536); offset=0; registry_writes=0
            while offset<len(events):
                _,_,_,length=struct.unpack_from('iIII',events,offset)
                name=events[offset+16:offset+16+length].split(b'\0')[0]
                if name==b'previews.json': registry_writes+=1
                offset+=16+length
            self.assertEqual(registry_writes,1,'project arrows publish current/selection in one save')
        finally: os.close(notify)
        arrow('Left','beta','beta-image',alt=True)
        self.assertEqual(self.projects()['alpha']['current'],'alpha-image')
        # Empty/ungrouped panes and space outside the grid leave all panes alone.
        for project in ('','empty',None):
            if project is None:
                input.motion(10,10); input.x.XFlush(input.display)
            else: hover(project)
            before=self.projects(); input.press('Right'); time.sleep(.15)
            self.assertEqual(self.projects(),before)
        hover('alpha')
        registry=self.fixture.state_dir/'previews.json'; backup=registry.with_suffix('.backup')
        registry.rename(backup); registry.mkdir()
        try:
            input.press('Right'); time.sleep(.2)
            self.assertEqual(self.projects()['alpha']['current'],'alpha-image')
            self.assertTrue(self.row('alpha-image')['active']); self.assertFalse(self.row('alpha-video')['active'])
        finally: registry.rmdir(); backup.rename(registry)
        arrow('Right','alpha','alpha-video')
        (self.root/'project-arrows-result.json').write_text(json.dumps({'renderer':renderer,'projects':self.projects()},indent=2))

    @unittest.skipUnless(harness.GPU,harness.GPU_REASON)
    def test_project_native_controls_scroll_and_layouts(self):
        self.rpc({'method':'theme','mode':'dark'})
        self.rpc({'method':'add','entry':{'id':'model','path':str(self.fixture.model),'project':'modeling','label':'Articulated model'}})
        self.send('texture','materials'); self.send('alternative','materials',False)
        path=self.root/'sound.wav'; audio(path)
        self.rpc({'method':'add','entry':{'id':'sound','path':str(path),'project':'sound','label':'Sound sketch'}})
        movie=self.root/'motion.mp4'; video(movie)
        self.rpc({'method':'add','entry':{'id':'video','path':str(movie),'project':'motion','label':'Motion study','settings':{'paused':True,'muted':True}}})
        self.rpc({'method':'project','id':'waiting','label':'Long project name waiting for its first export'})
        self.rpc({'method':'layout','layout':'projects','project_grid_size':2}); self.rpc({'method':'show'})
        model=self.loaded('model'); renderer=model['metrics']['renderer']
        self.assertFalse(any(token in renderer.lower() for token in ('llvmpipe','softpipe','software','unknown')),renderer)
        input=XInput(); self.addCleanup(input.close)
        input.x.XResizeWindow.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_uint,ctypes.c_uint]
        def resize(w,h):
            input.x.XResizeWindow(input.display,input.window,w,h); input.x.XFlush(input.display); time.sleep(.3)
        def capture(name):
            self.rpc({'method':'capture','path':str(self.root/(name+'.png'))})
        resize(1100,800); self.loaded('sound'); self.loaded('video'); capture('projects-2x2')
        # Native transport remains usable inside a project pane.
        r=self.row('sound')['controls']['play']; input.click(r['x']+r['width']//2,r['y']+r['height']//2)
        harness.eventually(lambda:not self.row('sound')['metrics']['paused'])
        input.click(r['x']+r['width']//2,r['y']+r['height']//2)
        harness.eventually(lambda:self.row('sound')['metrics']['paused'])
        time.sleep(.4); idle_start=ticks(self.state()['pid']); time.sleep(.4)
        idle_ticks=ticks(self.state()['pid'])-idle_start
        self.assertLessEqual(idle_ticks,5,'paused project panes must settle without polling')
        # Project chooser changes the pane, without switching global layouts.
        pane=next(p for p in self.state()['presentation']['projects']['panes'] if p['project']=='materials')
        r=pane['choice']; input.click(r['x']+r['width']//2,r['y']+r['height']//2)
        input.press('End'); input.press('Return')
        harness.eventually(lambda:self.projects()['materials']['current']=='alternative')
        self.assertEqual(self.state()['layout'],'projects')
        # Every preview keeps its full controls and surface; camera fit works.
        r=self.row('model')['controls']['reset']; input.click(r['x']+r['width']//2,r['y']+r['height']//2)
        for i in range(9): self.send(f'paint-{i}',f'project-{i}')
        self.rpc({'method':'layout','project_grid_size':3}); resize(1100,800); capture('projects-3x3')
        state=self.state(); self.assertGreater(state['presentation']['projects']['scroll_max'],0)
        before={r['id'] for r in state['entries'] if r['active']}
        pane=state['presentation']['projects']['panes'][0]; input.motion(pane['x']+30,pane['y']+100)
        for _ in range(18): input.scroll(pane['x']+30,pane['y']+40,up=False)
        harness.eventually(lambda:self.state()['presentation']['projects']['scroll']>0)
        after={r['id'] for r in self.state()['entries'] if r['active']}
        self.assertNotEqual(before,after); self.assertLess(len(after),len(self.projects()))
        capture('projects-scrolled')
        self.rpc({'method':'layout','project_grid_size':0}); resize(700,500); capture('projects-narrow')
        self.assertLessEqual(self.state()['presentation']['projects']['columns'],3)
        self.rpc({'method':'layout','layout':'overview','project_filter':'materials','group_projects':True}); capture('overview-filtered')
        self.assertFalse(self.state()['presentation']['overview']['native'])
        self.assertEqual(self.state()['presentation']['overview']['native_ancestors'],0)
        tiles=self.state()['presentation']['overview']['tiles']
        box=tiles[0]['checkbox']; input.click(box['x']+10,box['y']+10)
        input.key('Shift_L',1); box=tiles[1]['checkbox']; input.click(box['x']+10,box['y']+10); input.key('Shift_L',0); input.x.XFlush(input.display)
        harness.eventually(lambda:set(self.state()['presentation']['marked'])=={'texture','alternative'})
        self.rpc({'method':'layout','project_filter':''}); resize(1100,800); capture('overview-grouped')
        self.rpc({'method':'layout','group_projects':False}); capture('overview-flat')
        self.rpc({'method':'hide'}); self.assertEqual(self.state()['active'],0)
        (self.root/'projects-result.json').write_text(json.dumps({'renderer':renderer,'idle_ticks_400ms':idle_ticks,'final':self.state()},indent=2))

if __name__=='__main__':
    suite=unittest.TestSuite(ProjectsIntegration(name) for name in ProjectsIntegration.__dict__ if name.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
