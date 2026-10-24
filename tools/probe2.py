# -*- coding: utf-8 -*-
import sys
sys.stdout.reconfigure(encoding='utf-8')
import urllib3, requests
urllib3.disable_warnings()

PROXY = 'http://127.0.0.1:26561'
p = {'http': PROXY, 'https': PROXY}

urls = [
    'https://cdn.jsdelivr.net/gh/skywind3000/ECDICT@master/README.md',
    'https://ghproxy.net/https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md',
    'https://gh-proxy.com/https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md',
    'https://mirror.ghproxy.com/https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md',
    'https://gitee.com/explore',
    'https://github.com/skywind3000/ECDICT/releases',
    'https://objects.githubusercontent.com/',
    'https://pypi.tuna.tsinghua.edu.cn/simple/',
    'https://hf-mirror.com/',
]

for u in urls:
    try:
        r = requests.get(u, timeout=12, verify=False, proxies=p, stream=True)
        c = r.raw.read(200)
        print('OK  %-3d %-6d %s' % (r.status_code, len(c), u))
    except Exception as e:
        print('ERR %-22s %s' % (type(e).__name__ + ':' + str(e)[:60], u))
