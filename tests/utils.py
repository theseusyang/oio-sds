# Copyright (C) 2015-2017 OpenIO SAS, as part of OpenIO SDS
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3.0 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library.

from __future__ import print_function

import logging
import sys
import os
import random
import string

from subprocess import check_call
from functools import wraps
from urllib import urlencode

import yaml
import testtools

from oio.common.http_urllib3 import get_pool_manager
from oio.common.json import json as jsonlib
from oio.common.green import time

random_chars = string.ascii_letters + string.digits
random_chars_id = 'ABCDEF' + string.digits

CODE_NAMESPACE_NOTMANAGED = 418
CODE_SRVTYPE_NOTMANAGED = 453
CODE_POLICY_NOT_SATISFIABLE = 481


def ec(fnc):
    @wraps(fnc)
    def _wrapped(self):
        if len(self.conf['services']['rawx']) < 12:
            self.skipTest("Not enough rawx. "
                          "EC tests needs at least 12 rawx to run")
        fnc(self)
    return _wrapped


def random_str(size, chars=random_chars):
    return ''.join(random.choice(chars) for _ in range(size))


def random_id(size):
    return random_str(size, chars=random_chars_id)


def random_data(size):
    """Return `size` bytes of random data as a str object"""
    try:
        return os.urandom(size)
    except NotImplementedError:
        return random_str(size)


def trim_srv(srv):
    return {'score': srv['score'], 'addr': srv['addr'], 'tags': srv['tags']}


def get_config(defaults=None):
    conf = {}
    if defaults is not None:
        conf.update(defaults)

    default_conf_path = os.path.expanduser('~/.oio/sds/conf/test.yml')
    conf_file = os.environ.get('SDS_TEST_CONFIG_FILE', default_conf_path)

    try:
        with open(conf_file, 'r') as f:
            conf = yaml.load(f)
    except SystemExit:
        if not os.path.exists(conf_file):
            reason = 'file not found'
        elif not os.access(conf_file, os.R_OK):
            reason = 'permission denied'
        else:
            reason = 'n/a'
            print('Unable to read test config %s (%s)' % (conf_file, reason),
                  file=sys.stderr)
    return conf


class CommonTestCase(testtools.TestCase):

    TEST_HEADERS = {'X-oio-req-id': '7E571D0000000000'}

    def is_running_on_public_ci(self):
        from os import getenv
        clues = (getenv("TRAVIS"), getenv("CIRCLECI"))
        return any(clue is not None for clue in clues)

    def _random_user(self):
        return "user-" + random_str(16, "0123456789ABCDEF")

    def get_service_url(self, srvtype, i=0):
        allsrv = self.conf['services'][srvtype]
        srv = allsrv[i]
        return srv['num'], srv['path'], srv['addr'], srv.get('uuid')

    def get_service(self, srvtype, i=0):
        num, path, addr, _ = self.get_service_url(srvtype, i=i)
        ip, port = addr.split(':')
        return num, path, ip, port

    def _url(self, name):
        return '/'.join((self.uri, "v3.0", self.ns, name))

    def _url_cs(self, action):
        return self._url("conscience") + '/' + action

    def _url_lb(self, action):
        return self._url("lb") + '/' + action

    def _url_ref(self, action):
        return self._url("reference") + '/' + action

    def url_container(self, action):
        return self._url("container") + '/' + action

    def url_content(self, action):
        return self._url("content") + '/' + action

    def param_srv(self, ref, srvtype):
        return {'ref': ref, 'acct': self.account, 'type': srvtype}

    def param_ref(self, ref):
        return {'ref': ref, 'acct': self.account}

    def param_content(self, ref, path):
        return {'ref': ref, 'acct': self.account, 'path': path}

    @staticmethod
    def static_request(method, url, data=None, params=None, headers=None,
                       json=None, http_pool=None):
        if not http_pool:
            http_pool = get_pool_manager()
        # Add query string
        if params:
            out_param = []
            for k, v in params.items():
                if v is not None:
                    if isinstance(v, unicode):
                        v = unicode(v).encode('utf-8')
                    out_param.append((k, v))
            encoded_args = urlencode(out_param)
            url += '?' + encoded_args

        # Convert json and add Content-Type
        headers = headers if headers else {}
        if json:
            headers["Content-Type"] = "application/json"
            data = jsonlib.dumps(json)

        out_kwargs = {}
        out_kwargs['headers'] = headers
        out_kwargs['body'] = data

        return http_pool.request(method, url, **out_kwargs)

    def request(self, method, url,
                data=None, params=None, headers=None, json=None):
        return self.static_request(method, url, data=data, params=params,
                                   headers=headers, json=json,
                                   http_pool=self.http_pool)

    @classmethod
    def setUpClass(cls):
        super(CommonTestCase, cls).setUpClass()
        cls._cls_conf = get_config()
        cls._cls_account = cls._cls_conf['account']
        cls._cls_ns = cls._cls_conf['namespace']
        cls._cls_uri = 'http://' + cls._cls_conf['proxy']
        # TODO(FVE): we should mix Apache and Go rawx services.
        # They could be in specific slots.
        some_rawx = cls._cls_conf['services']['rawx'][0]['addr']
        resp = cls.static_request('GET', 'http://%s/info' % some_rawx)
        cls._cls_conf['go_rawx'] = resp.headers.get('Server') != 'Apache'

    def setUp(self):
        super(CommonTestCase, self).setUp()
        self.conf = get_config()
        self.uri = 'http://' + self.conf['proxy']
        self.ns = self.conf['namespace']
        self.account = self.conf['account']
        queue_addr = random.choice(self.conf['services']['beanstalkd'])['addr']
        self.conf['queue_url'] = 'beanstalk://' + queue_addr
        self._beanstalk = None
        self._conscience = None
        self._http_pool = None

    def tearDown(self):
        super(CommonTestCase, self).tearDown()

    @classmethod
    def tearDownClass(cls):
        super(CommonTestCase, cls).tearDownClass()

    @property
    def conscience(self):
        if not self._conscience:
            from oio.conscience.client import ConscienceClient
            self._conscience = ConscienceClient(self.conf,
                                                pool_manager=self.http_pool)
        return self._conscience

    @property
    def http_pool(self):
        if not self._http_pool:
            self._http_pool = get_pool_manager()
        return self._http_pool

    @property
    def beanstalk(self):
        if not self._beanstalk:
            from oio.event.beanstalk import Beanstalk
            self._beanstalk = Beanstalk.from_url(self.conf['queue_url'])
        return self._beanstalk

    def _flush_cs(self, srvtype):
        params = {'type': srvtype}
        resp = self.request('POST', self._url_cs("flush"),
                            params=params, headers=self.TEST_HEADERS)
        self.assertEqual(resp.status / 100, 2)

    def _register_srv(self, srv):
        resp = self.request('POST', self._url_cs("register"),
                            jsonlib.dumps(srv), headers=self.TEST_HEADERS)
        self.assertIn(resp.status, (200, 204))

    def _lock_srv(self, srv):
        resp = self.request('POST', self._url_cs("lock"),
                            jsonlib.dumps(srv), headers=self.TEST_HEADERS)
        self.assertIn(resp.status, (200, 204))

    def _unlock_srv(self, srv):
        resp = self.request('POST', self._url_cs("unlock"),
                            jsonlib.dumps(srv), headers=self.TEST_HEADERS)
        self.assertIn(resp.status, (200, 204))

    def _flush_proxy(self):
        url = self.uri + '/v3.0/cache/flush/local'
        resp = self.request('POST', url, '', headers=self.TEST_HEADERS)
        self.assertEqual(resp.status / 100, 2)

    @classmethod
    def _cls_reload_proxy(cls):
        url = '{0}/v3.0/{1}/lb/reload'.format(cls._cls_uri, cls._cls_ns)
        cls.static_request('POST', url, '')

    def _reload_proxy(self):
        url = '{0}/v3.0/{1}/lb/reload'.format(self.uri, self.ns)
        resp = self.request('POST', url, '', headers=self.TEST_HEADERS)
        self.assertEqual(resp.status / 100, 2)

    def _flush_meta(self):
        for srvtype in ('meta1', 'meta2'):
            for t in self.conf['services'][srvtype]:
                url = self.uri + '/v3.0/forward/flush'
                resp = self.request('POST', url,
                                    params={'id': t['addr']},
                                    headers=self.TEST_HEADERS)
                self.assertEqual(resp.status, 204)

    @classmethod
    def _cls_reload_meta(cls):
        for srvtype in ('meta1', 'meta2'):
            for t in cls._cls_conf['services'][srvtype]:
                url = cls._cls_uri + '/v3.0/forward/reload'
                cls.static_request('POST', url, params={'id': t['addr']})

    def _reload_meta(self):
        for srvtype in ('meta1', 'meta2'):
            for t in self.conf['services'][srvtype]:
                url = self.uri + '/v3.0/forward/reload'
                resp = self.request('POST', url,
                                    params={'id': t['addr']},
                                    headers=self.TEST_HEADERS)
                self.assertEqual(resp.status, 204)

    def _reload(self):
        self._flush_proxy()
        self._flush_meta()
        self._reload_meta()
        self._reload_proxy()

    def _addr(self, low=7000, high=65535, ip="127.0.0.2"):
        return ip + ':' + str(random.randint(low, high))

    def _srv(self, srvtype, extra_tags={}, lowport=7000, highport=65535,
             ip="127.0.0.2"):
        outd = {'ns': self.ns,
                'type': str(srvtype),
                'addr': self._addr(low=lowport, high=highport, ip=ip),
                'score': random.randint(1, 100),
                'tags': {'stat.cpu': 1, 'tag.vol': 'test', 'tag.up': True}}
        if extra_tags:
            outd["tags"].update(extra_tags)
        return outd

    def assertIsError(self, body, expected_code_oio):
        self.assertIsInstance(body, dict)
        self.assertIn('status', body)
        self.assertIn('message', body)
        self.assertEqual(body['status'], expected_code_oio)

    def assertError(self, resp, code_http, expected_code_oio):
        self.assertEqual(resp.status, code_http)
        self.assertIsError(self.json_loads(resp.data), expected_code_oio)

    @classmethod
    def json_loads(cls, data):
        try:
            return jsonlib.loads(data)
        except ValueError:
            logging.info("Unparseable data: %s", str(data))
            raise


class BaseTestCase(CommonTestCase):

    def setUp(self):
        super(BaseTestCase, self).setUp()
        self._flush_cs('echo')

    def tearDown(self):
        super(BaseTestCase, self).tearDown()
        self._flush_cs('echo')

    def _service(self, name, action, wait=0, socket=None):
        """
        Execute a gridinit action on a service, and optionally sleep for
        some seconds before returning.
        :param name: The service upon which the command should be executed.
        :param action: The command to send. (E.g. 'start' or 'stop')
        :param wait: The amount of time in seconds to wait after the command.
        :param socket: The unix socket on which gridinit is listenting.
                        defaults to ~/.oio/sds/run/gridinit.sock
        """
        if not socket:
            socket = os.path.expanduser('~/.oio/sds/run/gridinit.sock')
        name = "%s-%s" % (self.ns, name)
        check_call(['gridinit_cmd', '-S', socket, action, name])
        if wait > 0:
            time.sleep(wait)

    @classmethod
    def tearDownClass(cls):
        super(BaseTestCase, cls).tearDownClass()
