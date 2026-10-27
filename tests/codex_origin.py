"""Origin metadata follows each caller, without retargeting read-only requests."""
import importlib.machinery,importlib.util,json,os,unittest
from pathlib import Path
from unittest.mock import patch,MagicMock
loader=importlib.machinery.SourceFileLoader('origin_client',str(Path(__file__).resolve().parents[1]/'bin/asset-preview'))
spec=importlib.util.spec_from_loader(loader.name,loader);client=importlib.util.module_from_spec(spec);loader.exec_module(client)
class Origin(unittest.TestCase):
 def test_each_request_carries_only_its_callers_origin(self):
  first='00000000-0000-4000-8000-000000000001';second='00000000-0000-4000-8000-000000000002'
  for thread in (first,second):
   for method in ('add','show','open','list'):
    request={'method':method,'entry':{'path':'/tmp/image.png'}}
    wire=MagicMock();wire.recv.return_value=b'{"ok":true}\n'
    with patch.dict(os.environ,{'CODEX_THREAD_ID':thread,'CODEX_HOME':'/tmp/codex-origin'},clear=True),patch.object(client.socket,'socket') as factory:
     factory.return_value.__enter__.return_value=wire;client.rpc(request)
    sent=json.loads(wire.sendall.call_args.args[0]);self.assertNotIn('codex',request);self.assertNotIn('codex',request['entry'])
    if method=='list':self.assertNotIn('codex',sent)
    else:
     origin=(sent['entry'] if method=='add' else sent)['codex']
     self.assertEqual(origin,{'thread_id':thread,'cwd':os.getcwd(),'socket':'/tmp/codex-origin/app-server-control/app-server-control.sock'})
 def test_invalid_thread_is_ignored_and_socket_override_preserved(self):
  with patch.dict(os.environ,{'CODEX_THREAD_ID':'invalid'},clear=True):self.assertEqual(client.codex_origin(),{})
  with patch.dict(os.environ,{'CODEX_SESSION_ID':'00000000-0000-4000-8000-000000000001','ASSET_PREVIEW_CODEX_SOCKET':'/tmp/test-codex.sock'},clear=True):self.assertEqual(client.codex_origin()['socket'],'/tmp/test-codex.sock')
if __name__=='__main__':unittest.main(verbosity=2)
