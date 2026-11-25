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

    def test_project_hierarchy_order_filters_restart_and_undo(self):
        self.send('character','characters'); self.send('sound','sounds'); self.send('prop','props')
        self.rpc({'method':'project','id':'game','label':'My Game'})
        self.rpc({'method':'project','id':'characters','parent':'game','label':'Characters'})
        self.rpc({'method':'project','id':'props','parent':'game','label':'Props'})
        self.assertEqual(list(self.projects()),['sounds','game','characters','props'])
        self.rpc({'method':'layout','layout':'overview','project_filter':'game'})
        self.rpc({'method':'show'})
        state=self.state()
        self.assertEqual(state['active'],0)
        self.assertEqual([g['id'] for g in state['presentation']['overview']['groups']],['game','characters','props'])
        self.assertEqual([g['nested'] for g in state['presentation']['overview']['groups']],[False,True,True])
        self.assertEqual(state['presentation']['overview']['items'],2)
        self.rpc({'method':'layout','layout':'gallery'})
        self.loaded('character')
        self.assertEqual(self.state()['active'],1)
        self.rpc({'method':'layout','project_filter':'props'})
        self.assertEqual(self.state()['selected'],'prop')
        self.assertEqual(self.state()['presentation']['overview']['items'],1)
        self.rpc({'method':'layout','layout':'projects','project_filter':'game'})
        self.assertEqual([p['project'] for p in self.state()['presentation']['projects']['panes']],['characters','props'])
        self.restart()
        self.assertEqual(self.projects()['characters']['parent'],'game')
        self.assertEqual(self.projects()['game']['label'],'My Game')
        self.rpc({'method':'close_all'}); self.assertEqual(self.projects(),{})
        self.rpc({'method':'reopen'})
        self.assertEqual(self.projects()['props']['parent'],'game')
        self.assertEqual(self.projects()['game']['label'],'My Game')
        self.restart()
        self.assertEqual(list(self.projects()),['game','props'])

    def test_project_parent_validation_and_transaction_rollback(self):
        path=self.send('character','characters')
        self.rpc({'method':'project','id':'game'})
        self.rpc({'method':'project','id':'characters','parent':'game'})
        before=self.state()
        for request in (
            {'method':'project','id':'characters','parent':'missing'},
            {'method':'project','id':'characters','parent':'characters'},
            {'method':'project','id':'game','parent':'characters'},
            {'method':'project','id':'nested','parent':'characters'},
            {'method':'project','id':'characters','parent':False},
            {'method':'project','id':'game','action':'remove'},
            {'method':'add','entry':{'id':'bad','path':str(path),'project':'new'},'parent_project':'characters'},
        ):
            with self.assertRaises(RuntimeError): self.rpc(request)
            self.assertEqual(before['projects'],self.state()['projects'])
        registry=self.fixture.state_dir/'previews.json'; backup=registry.with_suffix('.backup')
        registry.rename(backup); registry.mkdir()
        try:
            for request in (
                {'method':'project','id':'characters','parent':'','label':'Wrong'},
                {'method':'add','entry':{'id':'character','path':str(path),'project':'characters'},'parent_project':'other','parent_project_label':'Other'},
                {'method':'add','entry':{'id':'new','path':str(path),'project':'new-group'},'parent_project':'new-owner'},
            ):
                with self.assertRaises(RuntimeError): self.rpc(request)
                for key in ('projects','entries','project_filter','selected'):
                    self.assertEqual(before[key],self.state()[key],key)
        finally: registry.rmdir(); backup.rename(registry)
        self.rpc({'method':'project','id':'characters','parent':''})
        self.assertEqual(self.projects()['characters']['parent'],'')

    def test_group_registration_is_atomic_and_metadata_reconnect_is_saved(self):
        path=self.root/'waiting.png'
        request={'method':'add','entry':{'id':'asset','path':str(path),'project':'parts'},
                 'parent_project':'game','parent_project_label':'Game','project_label':'Parts'}
        self.rpc(request)
        self.assertEqual(self.projects()['parts']['parent'],'game')
        self.assertEqual(self.projects()['game']['label'],'Game')
        request['parent_project']='another'; request['parent_project_label']='Another'
        self.rpc(request)
        self.restart()
        self.assertEqual(self.projects()['parts']['parent'],'another')
        self.assertNotIn('game',self.projects())
        self.assertEqual(self.state()['window_visible'],False)

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
        self.rpc({'method':'close_all'}); self.assertEqual(self.projects(),{})
        self.assertEqual(self.state()['project_filter'],'')
        self.rpc({'method':'reopen'}); self.assertTrue(self.state()['entries'])

    def test_empty_projects_prune_on_close_move_bulk_and_undo(self):
        self.rpc({'method':'project','id':'alpha','label':'Original Alpha'})
        self.send('a'); self.send('support'); self.send('b','beta')
        self.rpc({'method':'layout','layout':'projects','project_filter':'alpha'})
        self.rpc({'method':'show'})
        self.rpc({'method':'remove','id':'a'})
        self.assertEqual(self.projects()['alpha']['current'],'support')
        self.rpc({'method':'remove','id':'support'})
        self.assertNotIn('alpha',self.projects())
        self.assertEqual(self.state()['project_filter'],'')
        self.assertEqual([p['project'] for p in self.state()['presentation']['projects']['panes']],['beta'])
        registry=self.fixture.state_dir/'previews.json'
        self.assertNotIn('alpha',{p['id'] for p in json.loads(registry.read_text())['projects']})
        self.rpc({'method':'reopen'})
        self.assertEqual(list(self.projects()),['alpha','beta'])
        self.assertEqual(self.projects()['alpha']['label'],'Original Alpha')
        # Moving the last member has the same ownership cleanup as closing it.
        self.rpc({'method':'add','entry':{'id':'support','path':str(self.root/'support.png'),'project':'beta'}})
        self.assertNotIn('alpha',self.projects())
        self.restart(); self.assertNotIn('alpha',self.projects())
        self.rpc({'method':'project','id':'beta','label':'Original Beta'})
        self.rpc({'method':'remove_many','ids':['b','support']})
        self.assertEqual(self.projects(),{})
        self.rpc({'method':'reopen'})
        self.assertEqual(self.projects()['beta']['label'],'Original Beta')
        self.rpc({'method':'project','id':'waiting','label':'Waiting for an export'})
        self.rpc({'method':'close_all'}); self.assertEqual(self.projects(),{})
        self.rpc({'method':'reopen'})
        self.assertEqual(self.projects()['beta']['label'],'Original Beta')
        self.restart(); self.assertEqual(list(self.projects()),['beta'])

    def test_waiting_groups_are_session_only_and_legacy_empty_groups_migrate(self):
        self.send('a')
        self.rpc({'method':'project','id':'waiting','label':'Current work'})
        self.rpc({'method':'layout','layout':'projects','project_filter':'waiting'})
        self.assertIn('waiting',self.projects())
        registry=self.fixture.state_dir/'previews.json'
        saved=json.loads(registry.read_text())
        self.assertEqual([p['id'] for p in saved['projects']],['alpha'])
        self.assertEqual(saved['project_filter'],'')
        self.restart(); self.assertNotIn('waiting',self.projects())
        self.assertEqual(self.state()['project_filter'],'')
        self.rpc({'method':'quit'}); self.fixture.process.wait(timeout=5)
        saved=json.loads(registry.read_text())
        saved['projects'] += [{'id':f'old-{i}','label':f'Old project {i}','current':''} for i in range(19)]
        saved['project_filter']='old-0'
        registry.write_text(json.dumps(saved))
        self.restart_stopped()
        self.assertEqual(list(self.projects()),['alpha'])
        self.assertEqual(self.state()['project_filter'],'')
        migrated=json.loads(registry.read_text())
        self.assertEqual([p['id'] for p in migrated['projects']],['alpha'])
        self.assertEqual(migrated['entries'],saved['entries'])
        self.restart(); self.assertEqual(list(self.projects()),['alpha'])

    def test_failed_close_or_move_keeps_projects_and_filters(self):
        self.send('a'); self.send('b','beta')
        self.rpc({'method':'layout','layout':'projects','project_filter':'alpha'})
        registry=self.fixture.state_dir/'previews.json'; backup=registry.with_suffix('.backup')
        before=self.state(); registry.rename(backup); registry.mkdir()
        try:
            for request in (
                {'method':'remove','id':'a'},
                {'method':'remove_many','ids':['a','b']},
                {'method':'close_all'},
                {'method':'add','entry':{'id':'a','path':str(self.root/'a.png'),'project':'beta'}},
            ):
                with self.assertRaises(RuntimeError): self.rpc(request)
                after=self.state()
                for key in ('projects','entries','project_filter','selected'):
                    self.assertEqual(before[key],after[key],key)
        finally: registry.rmdir(); backup.rename(registry)
        self.rpc({'method':'remove','id':'a'})
        before=self.state(); registry.rename(backup); registry.mkdir()
        try:
            with self.assertRaises(RuntimeError): self.rpc({'method':'reopen'})
            after=self.state()
            for key in ('projects','entries','project_filter','selected'):
                self.assertEqual(before[key],after[key],key)
        finally: registry.rmdir(); backup.rename(registry)
        self.rpc({'method':'reopen'}); self.assertEqual(list(self.projects()),['alpha','beta'])

    def test_close_all_undo_restores_project_names_and_order(self):
        self.send('a'); self.send('b','beta')
        self.rpc({'method':'project','id':'alpha','label':'Alpha named'})
        self.rpc({'method':'project','id':'beta','label':'Beta named'})
        self.rpc({'method':'project','id':'waiting'})
        self.rpc({'method':'close_all'}); self.assertEqual(self.projects(),{})
        self.rpc({'method':'reopen'}); self.rpc({'method':'reopen'})
        self.assertEqual(list(self.projects()),['alpha','beta'])
        self.assertEqual(self.projects()['alpha']['label'],'Alpha named')
        self.assertEqual(self.projects()['beta']['label'],'Beta named')

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
        for overlay in ('name','controls','status','triangles'):
            self.assertTrue(model['overlays'][overlay]['native_project_pane'],
                             'Native model chrome must be clipped by its project pane before surface creation')
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
