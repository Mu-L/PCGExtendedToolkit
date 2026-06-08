import subprocess, re, difflib, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

def strip_comments(src):
    out=[]; i=0; n=len(src); st='N'
    while i<n:
        c=src[i]; d=src[i+1] if i+1<n else ''
        if st=='N':
            if c=='"': out.append(c); st='S'; i+=1
            elif c=="'": out.append(c); st='C'; i+=1
            elif c=='/' and d=='/': st='L'; i+=2
            elif c=='/' and d=='*': st='B'; i+=2
            else: out.append(c); i+=1
        elif st=='S':
            out.append(c)
            if c=='\\': out.append(d); i+=2
            else:
                if c=='"': st='N'
                i+=1
        elif st=='C':
            out.append(c)
            if c=='\\': out.append(d); i+=2
            else:
                if c=="'": st='N'
                i+=1
        elif st=='L':
            if c=='\n': out.append(c); st='N'
            i+=1
        elif st=='B':
            if c=='*' and d=='/': st='N'; i+=2
            else: i+=1
    return ''.join(out)

def norm(src):
    src=src.lstrip('﻿')
    lines=[]
    for ln in strip_comments(src).split('\n'):
        ln=re.sub(r'\s+',' ',ln).strip().replace('﻿','')
        if ln: lines.append(ln)
    return lines

files=subprocess.check_output(['git','diff','--name-only'],text=True).split()
files=[f for f in files if f.endswith(('.h','.cpp'))]
bad=0; bom_lost=[]
for f in files:
    try:
        head=subprocess.check_output(['git','show',f'HEAD:{f}'],stderr=subprocess.DEVNULL).decode('utf-8','replace')
    except subprocess.CalledProcessError:
        print(f'NEW (no HEAD): {f}'); continue
    work=open(f,encoding='utf-8',errors='replace').read()
    if head.startswith('﻿') and not work.startswith('﻿'):
        bom_lost.append(f)
    a=norm(head); b=norm(work)
    if a==b:
        print(f'OK   {f}')
    else:
        bad+=1
        print(f'DIFF {f}  <-- CODE CHANGED')
        for line in difflib.unified_diff(a,b,lineterm='',n=1):
            if line[:1] in '+-' and line[:2] not in ('++','--'):
                print('     '+line)
print()
print(f'TOTAL files checked: {len(files)}  CODE-DIFF files: {bad}')
if bom_lost:
    print(f'BOM LOST in {len(bom_lost)} file(s):')
    for f in bom_lost: print('   '+f)
else:
    print('BOM: no losses')
