"""Create a separate first-stage modern-audio test package; never installs it.

Usage: python patches/prepare_recovery.py (--input Vince.exe | --game-dir <game folder>) [--output folder]
Includes the confirmed startup/exit fixes, the stereo music fix, plus a load-time import of VinceAudio.
Steps follow scripts/patch_status.py status(): A (prepare_patch) if absent, C (apply_c, the
VoodooVince_StereoMusicFix.bat apply rule) if absent, backend import, B (tail fix) if absent; since 2.3 T2 applied
from the patch spec (scripts/patchset.py apply_spec; compose_legacy keeps the old inline composition).
--game-dir reads Vince.exe, or Vince.exe.vinceaudio-backup when Vince.exe already has the loader (INSTALL.md).
--output defaults to build/package; an existing output is replaced only if it lies inside build/.
The output is self-checked (A/B/C/loader present, install check, input sections kept); on failure it is deleted.
It ships three empty presence-only switches beside the DLL: VinceAudio.shadow, VinceAudio.outage,
VinceAudio.rebuild (1.6; VinceAudio.trace is no longer shipped: off by default, INSTALL.md turns it on).
"""
import argparse
import hashlib
import runpy
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parent
BASE=runpy.run_path(str(ROOT/'prepare_patch.py'))
TAIL=runpy.run_path(str(ROOT/'vince_audio_tail_fix.py'))
END=runpy.run_path(str(ROOT/'vince_audio_end_fix.py'))
PATCH_STATUS=runpy.run_path(str(ROOT.parent/'scripts'/'patch_status.py'))
BUILD=ROOT.parent/'build'
BACKUP_NAME='Vince.exe.vinceaudio-backup' # pre-package exe, beside Vince.exe (patches/INSTALL.md)
# 2.1 T4: no switch file shipped; layers on by default, off-files `.noshadow/.nooutage/.norebuild` (docs/ops/switches.md).
# was: SWITCHES=('VinceAudio.shadow','VinceAudio.outage','VinceAudio.rebuild') shipped empty and on (1.4-1.6)
PACKAGE_FILES={'Vince.exe','VinceAudio.dll','TESTING.txt','INSTALL.md'}

def pre_package_exe(game_dir):
    """The exe to package from: Vince.exe if it has no loader, else the INSTALL.md backup (which must have none)."""
    exe=Path(game_dir)/'Vince.exe'
    if PATCH_STATUS['status'](exe)['loader']!='present':return exe
    backup=Path(game_dir)/BACKUP_NAME
    if not backup.exists():raise ValueError(f'{exe} has the VinceAudio loader and {backup} is missing')
    if PATCH_STATUS['status'](backup)['loader']!='absent':raise ValueError(f'{backup} has VinceAudio loader bytes')
    return backup

def patch_sites(a,b,c=None,d=False):
    """(rva,length) of the A sites if a, the B sites if b, the C code and reloc-block entries of exe bytes c, the D
    sites and caves if d (4.1 T3)."""
    sites=[]
    if a:sites+=[(BASE['STREAM_RVA'],len(BASE['ORIGINAL'])),(BASE['WAIT_CALL_RVA'],5),
                 (BASE['THREAD_SETUP_RVA'],len(BASE['THREAD_SETUP'])),(BASE['LATE_COUNT_RVA'],len(BASE['LATE_COUNT']))]
    if b:sites+=[(TAIL['SITE_VA']-TAIL['IMAGE_BASE'],10),(TAIL['CAVE_VA']-TAIL['IMAGE_BASE'],40)]
    if c is not None:
        fix=PATCH_STATUS['stereo_fix']();_,rva,slots,_,_=PATCH_STATUS['c_reloc_block'](c)
        sites+=[(fix['CodeRva'],len(fix['CodeNew'])),(rva+8,2*slots)]
    if d:sites+=[(va-END['IMAGE_BASE'],len(orig)) for _,va,orig,_ in END['sites']()]
    return sites

def apply_c(data):
    """Patch C by the VoodooVince_StereoMusicFix.bat apply rule (constants from the .bat); ValueError on any refusal."""
    fix=PATCH_STATUS['stereo_fix']();data=bytearray(data)
    rva,orig,new=fix['CodeRva'],fix['CodeOrig'],fix['CodeNew']
    at=PATCH_STATUS['c_code_at'](data)
    if data[at:at+len(new)]==new:raise ValueError('patch C already present')
    if data[at:at+len(orig)]!=orig:raise ValueError('patch C code is foreign')
    off,_,slots,page,entries=PATCH_STATUS['c_reloc_block'](data)
    keep=sorted({r for r in entries if r+4<=rva or r>=rva+len(new)}|{fix['NewReloc']})
    if len(keep)>slots:raise ValueError('patch C relocations do not fit their block')
    data[at:at+len(new)]=new
    data[off+8:off+8+2*slots]=struct.pack(f'<{slots}H',*[0x3000|(r-page) for r in keep],*[0]*(slots-len(keep)))
    return bytes(data)

def section_table(data):
    pe=struct.unpack_from('<I',data,0x3c)[0]
    count=struct.unpack_from('<H',data,pe+6)[0]
    table=pe+24+struct.unpack_from('<H',data,pe+20)[0]
    return [data[table+i*40:table+i*40+40] for i in range(count)]

def sections_kept(source,result,sites):
    """Problems where a section of source lost its header or raw bytes in result (patch sites excluded)."""
    problems=[];after=section_table(result)
    for i,header in enumerate(section_table(source)):
        name=header[:8].rstrip(b'\0').decode('ascii','replace')
        if i>=len(after) or after[i]!=header:problems.append(f'{name}: section header changed');continue
        rva,rsize,raw=struct.unpack_from('<III',header,12)
        a=bytearray(source[raw:raw+rsize]);b=bytearray(result[raw:raw+rsize])
        for site,n in sites:
            if rva<=site<rva+rsize:a[site-rva:site-rva+n]=b[site-rva:site-rva+n]
        if a!=b:problems.append(f'{name}: raw bytes changed')
    return problems

VAUDIO_NAME=b'.vaudio'
VAUDIO_CHARACTERISTICS=0xc0000040
IMPORT_HEADER_EDITS=(('NumberOfSections','add',1),('SizeOfInitializedData','add','SizeOfRawData'),
                     ('SizeOfImage','align','end'),('CheckSum','set',0),
                     ('DataDirectory[1].VirtualAddress','set','rva'),('DataDirectory[1].Size','set','directory'))

def import_blob(descriptors,new_rva):
    """.vaudio bytes at new_rva: the old descriptors, the VinceAudio one, terminator, names, lookup, IAT; and offsets."""
    old_count=len(descriptors)//20
    align=BASE['align']
    blob=bytearray(descriptors)+bytes(40) # new descriptor and terminator
    dll=len(blob);blob+=b'VinceAudio.dll\0'
    if len(blob)&1:blob+=b'\0'
    name=len(blob);blob+=b'\0\0VinceAudioBootstrap\0'
    blob+=bytes(align(len(blob),4)-len(blob))
    lookup=len(blob);blob+=struct.pack('<II',new_rva+name,0)
    iat=len(blob);blob+=struct.pack('<II',new_rva+name,0)
    struct.pack_into('<IIIII',blob,old_count*20,new_rva+lookup,0,0,new_rva+dll,new_rva+iat)
    return bytes(blob),{'new':old_count*20,'dll':dll,'name':name,'lookup':lookup,'iat':iat,
                        'directory':(old_count+2)*20}

def backend_import(source):
    data=bytearray(source)
    pe=struct.unpack_from('<I',data,0x3c)[0]
    opt=pe+24
    count=struct.unpack_from('<H',data,pe+6)[0]
    table=opt+struct.unpack_from('<H',data,pe+20)[0]
    sections=[]
    for i in range(count):
        at=table+i*40
        size,rva,n,raw=struct.unpack_from('<IIII',data,at+8)
        if data[at:at+8].rstrip(b'\0')==VAUDIO_NAME:raise ValueError('Backend already imported')
        sections.append((size,rva,n,raw))
    def offset(rva):
        for size,start,n,raw in sections:
            if start<=rva<start+n:return raw+rva-start
        raise ValueError(f'Unmapped RVA {rva:x}')
    header=table+count*40
    if header+40>min(raw for size,rva,n,raw in sections if n) or any(data[header:header+40]):
        raise ValueError('No empty section header available')
    section_align,file_align=struct.unpack_from('<II',data,opt+32)
    align=BASE['align']
    new_rva=align(max(rva+max(size,n) for size,rva,n,raw in sections),section_align)
    old_rva=struct.unpack_from('<I',data,opt+104)[0]
    pos=offset(old_rva)
    descriptors=bytearray()
    while any(data[pos:pos+20]):
        if len(descriptors)>4096:raise ValueError('Invalid import table')
        descriptors+=data[pos:pos+20];pos+=20
    blob,layout=import_blob(descriptors,new_rva)
    raw=align(len(data),file_align);n=align(len(blob),file_align)
    data.extend(bytes(raw+n-len(data)));data[raw:raw+len(blob)]=blob
    struct.pack_into('<8sIIIIIIHHI',data,header,VAUDIO_NAME,len(blob),new_rva,n,raw,0,0,0,0,VAUDIO_CHARACTERISTICS)
    BASE['header_apply'](data,pe,IMPORT_HEADER_EDITS,{'rva':new_rva,'end':new_rva+len(blob),'SizeOfRawData':n,
                                                      'directory':layout['directory']},section_align)
    return bytes(data)

def compose_legacy(data,work_dir):
    """The pre-2.3 composition of data: A if absent, C if absent, backend import, B by tail-fix subprocess if absent.
    Scratch files live in work_dir and are deleted; ValueError on a refusal (callers check partial/loader first)."""
    work=Path(work_dir);work.mkdir(parents=True,exist_ok=True)
    exe=work/'Vince.exe';exe.write_bytes(data);b_out=work/'Vince.b.exe'  # 1.3 T5: B into a new file (was in place + backup)
    try:
        s=PATCH_STATUS['status'](exe)
        stable=BASE['patch'](data)[0] if s['A']=='absent' else data
        if s['C']=='absent':
            try:stable=apply_c(stable)
            except ValueError as e:raise ValueError(f'patch C refused ({e})') from None
        exe.write_bytes(backend_import(stable))
        if s['B']=='absent':
            b_out.unlink(missing_ok=True)
            r=subprocess.run([sys.executable,'-I',str(ROOT/'vince_audio_tail_fix.py'),'--input',str(exe),'--output',str(b_out)],capture_output=True,text=True)
            if r.returncode or not b_out.exists():raise ValueError(f'vince_audio_tail_fix.py failed (exit {r.returncode}): {(r.stdout+r.stderr).strip()}')
            return b_out.read_bytes()
        return exe.read_bytes()
    finally:
        exe.unlink(missing_ok=True);b_out.unlink(missing_ok=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    source=parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--input',type=Path)
    source.add_argument('--game-dir',type=Path)
    parser.add_argument('--output',type=Path,default=BUILD/'package')
    args=parser.parse_args()
    dll=BUILD/'VinceAudio.dll'
    if not dll.exists():parser.error('Build VinceAudio.dll first with scripts/build.ps1')
    try:path=args.input or pre_package_exe(args.game_dir)
    except ValueError as e:sys.exit(f'{e}; refusing, nothing written')
    original=path.read_bytes()
    s=PATCH_STATUS['status'](path)
    for key in ('A','B','C','loader'):
        if s[key]=='partial':sys.exit(f'{path}: {key} is partial (foreign or half-applied bytes); refusing, nothing written')
    if s['loader']=='present':sys.exit(f'{path} already has the VinceAudio loader; give the pre-package exe. Nothing written')
    out=args.output.resolve()
    if out.exists():
        if BUILD.resolve() not in out.parents:parser.error('Output must be a new directory (or one inside build/)')
        shutil.rmtree(out)
    # 2.3 T2: A/C/loader/B from the patch spec (scripts/patchset.py apply_spec); was the inline compose_legacy().
    patchset=runpy.run_path(str(ROOT.parent/'scripts'/'patchset.py'))
    try:result=patchset['apply_spec'](original,*patchset['load_spec']())[0]
    except ValueError as e:sys.exit(f'{path}: {e}; nothing written')
    out.mkdir(parents=True)
    exe=out/'Vince.exe'
    exe.write_bytes(result)
    (out/'VinceAudio.dll').write_bytes(dll.read_bytes())
    readme=(ROOT/'TESTING.txt').read_text(encoding='utf-8')
    (out/'TESTING.txt').write_text(readme,encoding='utf-8')
    shutil.copyfile(ROOT/'INSTALL.md',out/'INSTALL.md')
    # Self-check of what was written.
    result=exe.read_bytes();problems=[]
    after=PATCH_STATUS['status'](exe)
    problems+=[f'{k}: {after[k]}' for k in ('A','B','C','D','loader') if after[k]!='present']
    ok,why=PATCH_STATUS['install_check'](result)
    if not ok:problems.append(f'install check: {why}')
    problems+=sections_kept(original,result,patch_sites(s['A']=='absent',s['B']=='absent',result if s['C']=='absent' else None,s['D']=='absent'))
    files={p.name for p in out.iterdir()}
    if files!=PACKAGE_FILES:problems.append(f'package files {sorted(files)}')
    if problems:
        shutil.rmtree(out)
        sys.exit('Self-check failed, package deleted: '+'; '.join(problems))
    print(f'Input:        {path}')
    print(f'Input SHA256: {hashlib.sha256(original).hexdigest()}')
    print(f"A:      {'applied' if s['A']=='absent' else 'kept'}")
    print('loader: applied')
    print(f"B:      {'applied' if s['B']=='absent' else 'kept'}")
    print(f"C:      {'applied' if s['C']=='absent' else 'kept'}")
    print('Self-check:   ok (A/B/C/loader present, install check pass, input sections kept)')
    print(f'EXE SHA256:   {hashlib.sha256(result).hexdigest()}')
    print(f'DLL SHA256:   {hashlib.sha256(dll.read_bytes()).hexdigest()}')
    print('Switches:     none shipped (layers on by default; off-files .noshadow/.nooutage/.norebuild)')
    print(f'Uninstalled experimental package: {out}')

if __name__=='__main__':main()
