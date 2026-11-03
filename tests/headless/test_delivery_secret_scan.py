"""Enum metadata is public; actual recipient IDs and credentials remain private."""
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from prepare_delivery_bundle import secret_scan
class Config:
    def read_text(self,encoding):
        return json.dumps({'feishu_notify_receive_id_type':'open_id',
            'feishu_notify_receive_id':'ou_synthetic_private_recipient',
            'feishu_app_secret':'synthetic_private_app_secret'})
class Scan(unittest.TestCase):
    def test_public_type(self):
        self.assertEqual(secret_scan({'doc.md':b'receive_id_type=open_id'},Config()),2)
    def test_recipient_is_still_private(self):
        with self.assertRaises(RuntimeError):
            secret_scan({'doc.md':b'ou_synthetic_private_recipient'},Config())
    def test_secret_is_still_private(self):
        with self.assertRaises(RuntimeError):
            secret_scan({'doc.md':b'synthetic_private_app_secret'},Config())
if __name__=='__main__':unittest.main()
