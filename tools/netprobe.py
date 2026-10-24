# -*- coding: utf-8 -*-
import sys, socket

results = []
PROXY = 'http://127.0.0.1:26561'

# 1) TCP connect test to proxy
for host, port in [('127.0.0.1', 26561)]:
    try:
        s = socket.create_connection((host, port), timeout=5)
        s.close()
        results.append(('tcp %s:%d' % (host, port), 'OK'))
    except Exception as e:
        results.append(('tcp %s:%d' % (host, port), 'ERR ' + type(e).__name__ + ' ' + str(e)[:60]))

import urllib3
urllib3.disable_warnings()
import requests

proxies = {'http': PROXY, 'https': PROXY}
for u in ['https://pypi.org/simple/', 'https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md', 'https://www.baidu.com/']:
    try:
        r = requests.get(u, timeout=15, verify=False, proxies=proxies)
        results.append(('PROXY GET ' + u, 'OK %d len=%d' % (r.status_code, len(r.content))))
    except Exception as e:
        results.append(('PROXY GET ' + u, 'ERR ' + type(e).__name__ + ' ' + str(e)[:70]))

out = '\n'.join('%-60s %s' % (k, v) for k, v in results)
sys.stdout.write(out + '\n')
sys.stdout.flush()
