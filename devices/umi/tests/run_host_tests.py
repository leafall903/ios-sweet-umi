#!/usr/bin/env python3
"""Run the v6 production parser/span/image functions without Apple software."""
from pathlib import Path
import argparse, os, re, shutil, subprocess, tempfile
B=Path(__file__).resolve().parent
GL=B.parent/'userland/gl/eagl.c'

def function(code,name):
    p=re.search(r'(?m)^static (?:struct image \*|void)\s*\n'+name+r'\(',code)
    if not p:raise ValueError('Missing production function '+name)
    brace=code.index('{',p.start());depth=1;i=brace+1
    while depth:
        depth+=(code[i]=='{')-(code[i]=='}');i+=1
    return code[p.start():i]+'\n'

def main(sanitize):
    cc=shutil.which(os.environ.get('CC','clang'));cpp=shutil.which(os.environ.get('CXX','clang++'))
    if not cc or not cpp:raise SystemExit('clang and clang++ are required')
    code=GL.read_text();fixture=(B/'test_images.c').read_text()
    first=fixture.index('static struct image *\nimage_of(');last=fixture.index('#undef calloc',first)
    fixture=fixture[:first]+'\n'.join(function(code,n) for n in ['image_of','image_new','image_forget','image_dispose'])+fixture[last:]
    capacity=re.search(r'^#define UMI_EAGL_IMAGE_CAPACITY (\d+)',code,re.M).group(1)
    fixture=re.sub(r'#define UMI_EAGL_IMAGE_CAPACITY \d+','#define UMI_EAGL_IMAGE_CAPACITY '+capacity,fixture)
    with tempfile.TemporaryDirectory(prefix='umi-v6-tests-') as tmp:
        tmp=Path(tmp);image=tmp/'images.c';image.write_text(fixture)
        for name,source,compiler in [('touch',B/'test_touch.cpp',cpp),('span',B/'test_span.cpp',cpp),('images',image,cc)]:
            flags=['-O1','-g','-I'+str(B)]
            if compiler==cpp:flags+=['-std=c++17']
            if sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run([compiler,*flags,str(source),'-o',str(tmp/name)],check=True)
            subprocess.run([str(tmp/name)],check=True)
    print('v6 host regressions passed; hardware/UI/FPS are outside these tests')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');a=p.parse_args();main(a.sanitize)
