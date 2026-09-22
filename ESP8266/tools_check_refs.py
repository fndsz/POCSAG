import re

s = open('update_refactored.html', encoding='gb18030').read()
js = re.search(r'<script>(.*?)</script>', s, re.S).group(1)

# $('xxx') 简写
refs = set(re.findall(r"\$\(['\"]([A-Za-z_]\w*)['\"]\)", js))
ids  = set(re.findall(r'id="([^"]+)"', s))

print('$(), 引用:', sorted(refs))
print('HTML 定义:', sorted(ids))
print()
print('引用但未定义:', sorted(refs - ids) or '无')
print('定义但未引用:', sorted(ids - refs))
