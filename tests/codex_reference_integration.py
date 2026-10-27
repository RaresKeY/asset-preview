#!/usr/bin/env python3
"""Opt-in live Codex + native input test. Runs only inside tools/offscreen.sh.
Uses throwaway conversations on the existing same-user Codex server.
No production preview registrations or prompts are touched.
"""
from pathlib import Path
import ctypes
import json
import os
import shlex
import subprocess
import tempfile
import time
import sys

import integration as h
from codex_rpc import Codex

ROOT=Path(__file__).resolve().parents[1]
EVIDENCE=Path(os.environ.get('ASSET_PREVIEW_EVIDENCE_DIR',Path.home()/'Pictures/Asset Preview/asset-preview-codex-20261021'))

PROBE=r'''#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QScreen>
#include <QTimer>
#include <cstdio>
int main(int argc,char** argv) {
 QApplication app(argc,argv);
 QTimer::singleShot(0,[&] {
  const QString action=argv[1];
  if(action=="text" || action=="image") {
  if(action=="text") { QFile f(argv[2]); if(!f.open(QIODevice::ReadOnly)) { app.exit(2); return; } app.clipboard()->setText(QString::fromUtf8(f.readAll())); }
  else app.clipboard()->setImage(QImage(argv[2]));
  std::puts("ready"); std::fflush(stdout); QTimer::singleShot(60000,&app,&QApplication::quit);
 }
  else if(action=="screen") { app.primaryScreen()->grabWindow(0).save(argv[2]); app.quit(); }
  else { const auto image=app.clipboard()->image(); const auto* mime=app.clipboard()->mimeData();
   auto json=QJsonDocument(QJsonObject{{"width",image.width()},{"height",image.height()},{"text",mime->hasText()},{"urls",mime->hasUrls()},{"alpha",image.pixelColor(0,0).alpha()}}).toJson(QJsonDocument::Compact);
   std::puts(json.constData()); app.quit(); }
 }); return app.exec();
}'''

def main():
    assert os.environ.get('QT_QPA_PLATFORM')=='xcb','Run this with tools/offscreen.sh'
    EVIDENCE.mkdir(parents=True,exist_ok=True)
    report={'checks':[],'captures':[]}
    socket=Path(os.environ.get('ASSET_PREVIEW_CODEX_SOCKET',Path.home()/'.codex/app-server-control/app-server-control.sock'))
    c=Codex(socket)
    input=None; clip=None; threads=[]
    with tempfile.TemporaryDirectory(prefix='preview-codex-live-') as folder:
        fixture=Path(folder)
        os.environ['ASSET_PREVIEW_RUNTIME_DIR']=str(fixture/'runtime')
        os.environ['ASSET_PREVIEW_STATE_DIR']=str(fixture/'state')
        os.environ['QT_XCB_NO_XI2']='1'
        os.environ['ASSET_PREVIEW_TEST_NULL_AUDIO']='1'
        # Explicit origins below, independent of the test runner's conversation.
        os.environ.pop('CODEX_THREAD_ID',None);os.environ.pop('CODEX_SESSION_ID',None)
        source=fixture/'clipboard.cpp';source.write_text(PROBE);probe=fixture/'clipboard'
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets'],text=True))
        subprocess.run(['c++','-std=c++20','-fPIC',str(source),'-o',str(probe),*flags],check=True)
        def check(name,condition):
            assert condition,name
            report['checks'].append(name);print('PASS',name,flush=True)
        def snap(name,screen=False):
            path=EVIDENCE/(name+'.png');path.unlink(missing_ok=True)
            h.client.rpc({'method':'capture','path':str(path)})
            report['captures'].append(str(path));return path
        def presentation():return h.client.rpc({'method':'list'})['presentation']
        def click(name,editor=False):
            controls=presentation()['reference_controls'];box=(controls['editor'] if editor else controls)[name]
            input.click(box['x']+box['width']//2,box['y']+box['height']//2);time.sleep(.2)
        def terminal():
            box=presentation()['codex'];input.click(box['x']+20,box['y']+box['height']-50);time.sleep(.1)
        def paste(text):
            nonlocal clip
            if clip:clip.terminate();clip.wait(timeout=3)
            path=fixture/'paste.txt';path.write_text(text)
            clip=subprocess.Popen([str(probe),'text',str(path)],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
            assert clip.stdout.readline().strip()=='ready'
            terminal();input.chord('Control_L','Shift_L','v');time.sleep(.4)
        def close_window():
            class Data(ctypes.Union):_fields_=[('b',ctypes.c_char*20),('s',ctypes.c_short*10),('l',ctypes.c_long*5)]
            class Message(ctypes.Structure):_fields_=[('type',ctypes.c_int),('serial',ctypes.c_ulong),('sent',ctypes.c_int),('display',ctypes.c_void_p),('window',ctypes.c_ulong),('message_type',ctypes.c_ulong),('format',ctypes.c_int),('data',Data)]
            class Event(ctypes.Union):_fields_=[('message',Message),('pad',ctypes.c_long*24)]
            input.x.XInternAtom.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int];input.x.XInternAtom.restype=ctypes.c_ulong
            input.x.XSendEvent.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)]
            event=Event();event.message.type=33;event.message.display=input.display;event.message.window=input.window;event.message.format=32
            event.message.message_type=input.x.XInternAtom(input.display,b'WM_PROTOCOLS',False)
            event.message.data.l[0]=input.x.XInternAtom(input.display,b'WM_DELETE_WINDOW',False)
            assert input.x.XSendEvent(input.display,input.window,False,0,ctypes.byref(event))
            input.x.XFlush(input.display)
            h.eventually(lambda:not h.client.rpc({'method':'list'})['window_visible'])
        seed_turn=None
        def thread_turns():
            try:return list(reversed(c.rpc('thread/turns/list',{'threadId':thread,'limit':20,'itemsView':'full'})['data']))
            except RuntimeError as error:
                if any(text in str(error) for text in ('not materialized','is empty')):return []
                raise
        def read():return {'turns':[turn for turn in thread_turns() if turn['id']!=seed_turn]}
        def last_done():
            turns=read().get('turns',[])
            return turns and turns[-1]['status']=='completed'
        def users():
            return [i for turn in read().get('turns',[]) for i in turn.get('items',[]) if i.get('type')=='userMessage']
        try:
            thread=c.rpc('thread/start',{'cwd':str(ROOT),'ephemeral':False,'historyMode':'paginated','approvalPolicy':'never','sandbox':'read-only',
              'baseInstructions':'You are an integration test fixture. Follow the test prompts precisely, do not edit files, and keep replies brief. You may run the explicitly requested wait command.'})['thread']['id'];threads.append(thread)
            other=c.rpc('thread/start',{'cwd':str(ROOT),'ephemeral':False,'historyMode':'paginated','approvalPolicy':'never','sandbox':'read-only','baseInstructions':'Test fixture. No file changes.'})['thread']['id'];threads.append(other)
            c.rpc('turn/start',{'threadId':other,'input':[{'type':'text','text':'Reply SECOND_READY only.'}]})
            seed_turn=c.rpc('turn/start',{'threadId':thread,'input':[{'type':'text','text':'Reply READY only.'}]})['turn']['id']
            h.eventually(lambda:any(t['id']==seed_turn and t['status']=='completed' for t in thread_turns()),timeout=120)
            origin={'thread_id':thread,'cwd':str(ROOT),'socket':str(socket)}
            image=fixture/'transparent.png';h.png(image,(30,170,220,100),128)
            h.client.start();h.client.rpc({'method':'add','entry':{'id':'image-ref','path':str(image),'codex':origin}})
            h.client.rpc({'method':'show'});h.eventually(lambda:h.row('image-ref').get('metrics',{}).get('loads'))
            input=h.XInput()
            # Clipboard copies image pixels even after pan/zoom.
            view=h.row('image-ref')['viewport'];input.scroll(view['x']+100,view['y']+100);time.sleep(.25)
            input.chord('Control_L','Shift_L','c');time.sleep(.2)
            copied=json.loads(subprocess.check_output([str(probe),'read'],text=True))
            check('copy image has exact dimensions, transparency and no text/URLs',copied=={'width':128,'height':128,'text':False,'urls':False,'alpha':100})
            click('codexToggle');h.eventually(lambda:presentation()['codex'].get('connected'));time.sleep(4)
            check('sidebar attaches to originating thread',presentation()['codex']['thread_id']==thread)
            pid=presentation()['codex']['client_pid'];snap('01-terminal-attached')
            # Native Ctrl+V retrieves the copied image inside Codex.
            terminal();input.chord('Control_L','v');time.sleep(.7)
            snap('02-ctrl-v-image-draft')
            paste('Describe the main color of the attached image. Reply briefly beginning IMAGE_PASTE_OK.')
            input.press('Return');h.eventually(lambda:len(users())>=1,timeout=20)
            h.eventually(last_done,timeout=120)
            first=users()[0]
            check('Ctrl+V sends an actual image attachment',any(x.get('type') in ('image','localImage') for x in first.get('content',[])))
            # Draft survives close/collapse and keyboard shortcuts stay in terminal.
            paste('DRAFT_SURVIVES f e m 1 ');input.press('Left');input.press('Right')
            check('terminal typing does not navigate or close asset',h.client.rpc({'method':'list'})['selected']=='image-ref')
            click('codexCollapse');check('collapse keeps client alive',presentation()['codex']['client_pid']==pid and not presentation()['codex']['visible'])
            click('codexToggle');check('reopen reuses client and draft',presentation()['codex']['client_pid']==pid);snap('02b-restored-draft')
            terminal();input.chord('Control_L','u');time.sleep(.15)
            # Freeze the actual interface, annotate several tools and attach it.
            click('referenceWindow');h.eventually(lambda:'editor' in presentation()['reference_controls'])
            editor=presentation()['reference_controls']['editor'];box=editor['referenceCanvas']
            input.drag(box['x']+box['width']//5,box['y']+box['height']//3,150,110)
            click('Highlight',True);input.drag(box['x']+box['width']//4,box['y']+box['height']//2,160,60)
            click('Number',True);input.click(box['x']+box['width']//2,box['y']+box['height']//2);input.click(box['x']+box['width']//2+80,box['y']+box['height']//2)
            click('Pen',True);input.drag(box['x']+box['width']//3,box['y']+box['height']//3,65,50)
            click('Undo',True);snap('03-window-annotations',True)
            before=set((Path.home()/'Pictures/Asset Preview').glob('Preview-window-*.png'))
            click('Attach to Codex',True);h.eventually(lambda:'editor' not in presentation()['reference_controls'])
            saved=list(set((Path.home()/'Pictures/Asset Preview').glob('Preview-window-*.png'))-before)
            check('annotated reference saved under Pictures/Asset Preview',len(saved)==1)
            report['captures'].append(str(saved[0]));snap('04-annotated-image-draft')
            paste('Confirm the numbered dots and circle in the attached Preview screenshot. Reply briefly beginning ANNOTATION_OK.')
            input.press('Return');h.eventually(lambda:len(users())>=2,timeout=20);h.eventually(last_done,timeout=120)
            check('annotated screenshot arrives as image plus asset context',any(x.get('type') in ('image','localImage') for x in users()[1].get('content',[])))
            # A real running turn, followed by two native Tab queued messages.
            paste('Run python3 -c "import time; time.sleep(30)" with the shell tool now, then reply WAIT_DONE. Do not modify files.')
            input.press('Return');h.eventually(lambda:len(users())>=3,timeout=20)
            h.eventually(lambda:c.rpc('thread/read',{'threadId':thread,'includeTurns':False})['thread']['status']['type']=='active',timeout=20)
            paste('Reply QUEUED_ONE only.');input.press('Tab');time.sleep(.5)
            click('referenceWindow');h.eventually(lambda:'editor' in presentation()['reference_controls']);click('Attach to Codex',True)
            h.eventually(lambda:'editor' not in presentation()['reference_controls'])
            paste('Reply QUEUED_TWO only.');input.press('Tab');time.sleep(.5)
            snap('05-queued-prompts')
            # Another asset must not silently retarget the open conversation.
            h.client.rpc({'method':'add','entry':{'id':'other-ref','path':str(image),'codex':dict(origin,thread_id=other)}})
            h.client.rpc({'method':'select','id':'other-ref'})
            check('navigation keeps the terminal pinned',presentation()['codex']['thread_id']==thread)
            click('codexReconnect');h.eventually(lambda:presentation()['codex']['thread_id']==other)
            h.client.rpc({'method':'select','id':'image-ref'});click('codexReconnect')
            h.eventually(lambda:presentation()['codex']['thread_id']==thread)
            check('Follow asset retains the original terminal and its queued prompts',presentation()['codex']['client_pid']==pid)
            click('codexCollapse');close_window()
            check('closing Preview suspends asset resources',h.client.rpc({'method':'list'})['active']==0)
            check('window close keeps the terminal client alive',presentation()['codex']['client_pid']==pid)
            h.eventually(lambda:len(users())>=5 and last_done(),timeout=180)
            completed_turns=read()['turns'];check('background work and both queued turns finish after window closes',len(completed_turns)>=5 and all(t['status']=='completed' for t in completed_turns))
            queued_users=users()[3:]
            check('queued prompts execute in order',len(queued_users)==2 and 'QUEUED_ONE' in json.dumps(queued_users[0]) and 'QUEUED_TWO' in json.dumps(queued_users[1]))
            check('queued screenshot arrives as an actual image',any(x.get('type') in ('image','localImage') for x in queued_users[1]['content']))
            # A fresh running server turn also survives actual Preview shutdown.
            c.rpc('turn/start',{'threadId':thread,'input':[{'type':'text','text':'Run python3 -c "import time; time.sleep(5)" then reply EXIT_SURVIVED. Do not edit files.'}]})
            h.client.rpc({'method':'quit'});h.eventually(lambda:not h.client.running())
            h.eventually(lambda:len(users())>=6 and last_done(),timeout=120)
            check('running Codex work survives actual Preview process exit',read()['turns'][-1]['status']=='completed')
            completed_turns=read()['turns']
            report['turn_statuses']=[t['status'] for t in completed_turns]
            report['messages']=[i.get('text','') for t in completed_turns for i in t.get('items',[]) if i.get('type')=='agentMessage']
        finally:
            if clip:clip.terminate();clip.wait(timeout=3)
            if input:input.close()
            if h.client.running():h.client.rpc({'method':'quit'});h.eventually(lambda:not h.client.running())
            log=fixture/'state/server.log'
            if log.exists():(EVIDENCE/'server.log').write_text(log.read_text())
            for thread_id in threads:
                try:c.rpc('thread/archive',{'threadId':thread_id})
                except Exception:pass
            c.close()
            (EVIDENCE/'verification.json').write_text(json.dumps(report,indent=2))
    print('LIVE REFERENCE CHECKS PASSED',flush=True)

if __name__=='__main__':main()
