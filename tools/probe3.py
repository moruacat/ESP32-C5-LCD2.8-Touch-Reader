# -*- coding: utf-8 -*-
import sys, urllib3, requests
urllib3.disable_warnings()

PROXY = 'http://127.0.0.1:26561'
p = {'http': PROXY, 'https': PROXY}

urls = [
    'https://pypi.org/pypi/pyecdict/json',
    'https://files.pythonhosted.org/',
    'https://cdn.jsdelivr.net/gh/skywind3000/ECDICT@master/README.md',
    'https://ghproxy.net/https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md',
    'https://gh-proxy.com/https://raw.githubusercontent.com/skywind3000/ECDICT/master/README.md',
    'https://hf-mirror.com/',
    'https://huggingface.co/',
    'https://gitee.com/',
    'https://codeload.github.com/skywind3000/ECDICT/zip/refs/heads/master',
    'https://www.baidu.com/',
]

lines = []
for u in urls:
    try:
        r = requests.get(u, timeout=15, verify=False, proxies=p, stream=True)
        c = r.raw.read(120)
        lines.append('OK  %-4d %-8d %s' % (r.status_code, len(c), u))
        lines.append('    head: ' + repr(c[:80]))
    except Exception as e:
        lines.append('ERR %-30s %s' % (type(e).__name__ + ':' + str(e)[:50], u))

with open('tools/probe3.out.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
print('done')
