#!/usr/bin/env python3
"""Check the staged/tracked source checkpoint and its declared v6 correspondence."""
from pathlib import Path
import hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[1]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    paths=subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0')
    blocked={'.img','.ipsw','.dmg','.im4p','.adt','.dtb','.fd','.bin','.dylib','.o','.cpo','.a','.png','.jpg','.jpeg','.heic'}
    errors=[];count=0
    for name in filter(None,paths):
        p=ROOT/name
        if not p.is_file():continue
        count+=1;b=p.read_bytes()
        if p.suffix.lower() in blocked:errors.append(name+': non-source artifact')
        if b'\0' in b:errors.append(name+': binary content')
        if re.search(rb'(?:ghp_[A-Za-z0-9]{25,}|github_pat_[A-Za-z0-9_]{25,}|BEGIN (?:OPENSSH|RSA|EC) PRIVATE KEY)',b):errors.append(name+': possible credential')
        if name.startswith('devices/umi/'):
            if b'/Users/' in b or b'/private/tmp/' in b:errors.append(name+': private host path')
            if p.name=='TargetConditionals.h':errors.append(name+': SDK header')
            if '/performance-v7/' in name or '/features-v7/' in name:errors.append(name+': untested candidate')
    d=ROOT/'devices/umi';m=json.loads((d/'manifests/v6-source.json').read_text())
    assert m['release']=='umi-ios16.5-v6' and len(m['matched_stable_archive_units'])==18
    assert m['kernel_base']['commit']=='1b191cb58250d0705d8a51287127505aa4bc0789'
    assert sha(d/'kernel/umi-v6.patch')==m['kernel_patch_sha256']
    commands=json.loads((d/'manifests/v6-build-commands.json').read_text())
    assert {r['object'] for r in commands['units']}=={r['object'] for r in m['matched_stable_archive_units']}
    for unit in m['matched_stable_archive_units']:
        for name,digest in unit['files'].items():assert sha(ROOT/unit['unit']/name)==digest,(unit['object'],name)
    for name,digest in m['userland_source_files'].items():assert sha(ROOT/name)==digest,name
    mesa=json.loads((d/'userland/mesa/source.json').read_text())
    assert sha(d/'userland/mesa/mesa-25.0.7-umi-v6.patch')==mesa['patch_sha256']
    template=(d/'kernel/root-volume-policy.h.in').read_text()
    assert 'id[8]={0,0,0,0,0,0,0,0}' in template
    patch=(d/'kernel/umi-v6.patch').read_bytes()
    assert not re.search(rb'^\+\+\+ b/[^\n]*TargetConditionals.h',patch,re.M)
    assert not re.search(rb'^\+\+\+ b/[^\n]*UmiRootVolume.h$',patch,re.M)
    assert (ROOT/'README.md').read_text().startswith('# Xiaomi Mi 10')
    assert (ROOT/'README.zh-CN.md').is_file()
    for header,unit in [('UmiFtsCore.h','UmiFtsTouch'),('UmiImageSpan.h','SweetSurface')]:
        assert (d/'tests'/header).read_bytes()==(d/'kernel/units'/unit/header).read_bytes()
    for p in ROOT.rglob('*.md'):
        if '.git' in p.parts:continue
        for target in re.findall(r'\]\(([^)]+)\)',p.read_text()):
            if '://' in target or target.startswith(('#','mailto:')):continue
            local=target.split('#')[0]
            if local and not (p.parent/local).exists():errors.append(str(p.relative_to(ROOT))+': broken link '+target)
    if errors:raise SystemExit('\n'.join(errors))
    print(f'Publication checks passed: {count} tracked source files; 18 matched v6 units; English default README')

if __name__=='__main__':main()
