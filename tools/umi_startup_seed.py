"""Fresh research-root startup state for the SWEET AppleKeyStore stub.

This creates the public SWEET placeholder, never copies a device keybag.
The caller must bind a new, mounted image to its source/hash proof first.
"""
from pathlib import Path, PurePosixPath
import base64
import hashlib
import os
import plistlib
import stat

DIRECTORIES = {
    '/private/var/db/diagnostics': (272, 272, 0o751),
    '/private/var/db/uuidtext': (272, 272, 0o755),
    '/private/var/db/diagnostics/timesync': (272, 272, 0o755),
    '/private/var/keybags': (0, 0, 0o700),
}
KEYBAG = '/private/var/keybags/systembag.kb'


def require(value, message):
    if not value:
        raise ValueError(message)


def guest(root, relative, follow_final=True):
    root = Path(root).resolve()
    todo, parts, links = list(PurePosixPath(relative).parts), [], 0
    while todo:
        item = todo.pop(0)
        if item in ('/', '', '.'):
            continue
        if item == '..':
            require(parts, 'Guest path escapes root')
            parts.pop()
            continue
        p = root.joinpath(*parts, item)
        if p.is_symlink() and (follow_final or todo):
            links += 1
            require(links <= 40, 'Guest symlink loop')
            target = os.readlink(p)
            if target.startswith('/'):
                parts = []
            todo = list(PurePosixPath(target).parts) + todo
        else:
            parts.append(item)
    return root.joinpath(*parts)


def placeholder():
    return plistlib.dumps({'KeyBagKeys': b'SWEET'}, fmt=plistlib.FMT_XML)


def make_plan(root, version='16.5', build='20F66'):
    root = Path(root).resolve()
    require(root != Path('/'), 'Host root is forbidden')
    system = plistlib.loads(guest(root, '/System/Library/CoreServices/SystemVersion.plist').read_bytes())
    require((system.get('ProductVersion'), system.get('ProductBuildVersion')) == (version, build),
            'Unexpected source version')
    files = []
    for folder, (uid, gid, mode) in DIRECTORIES.items():
        p = guest(root, folder, False)
        require(not os.path.lexists(p) or (p.is_dir() and not p.is_symlink()),
                'Startup directory is not a real directory')
        if folder != '/private/var/keybags':
            files.append((folder + '/.migrated', b'', 272, 272, 0o644))
    files.append((KEYBAG, placeholder(), 0, 0, 0o600))
    rows = []
    for name, data, uid, gid, mode in files:
        p = guest(root, name, False)
        exists = os.path.lexists(p)
        if exists:
            require(p.is_file() and not p.is_symlink(), 'Existing seed is not a real file')
            require(p.read_bytes() == data, 'Refusing to overwrite unknown startup state')
        rows.append({'path': name, 'data_b64': base64.b64encode(data).decode(),
                     'sha256': hashlib.sha256(data).hexdigest(), 'uid': uid, 'gid': gid,
                     'mode': mode, 'already_exact': exists})
    return {'root': str(root), 'version': version, 'build': build, 'files': rows,
            'directories': DIRECTORIES, 'device_key_material_copied': False}


def apply(root, plan):
    root = Path(root).resolve()
    require(os.geteuid() == 0 and root.is_mount() and str(root) == plan['root'],
            'An already-root caller and bound mounted image are required')
    require(make_plan(root, plan['version'], plan['build']) == plan, 'Startup state changed')
    for name, (uid, gid, mode) in DIRECTORIES.items():
        p = guest(root, name)
        p.mkdir(parents=True, exist_ok=True)
        require(p.stat().st_dev == root.stat().st_dev, 'Directory outside image')
        os.chown(p, uid, gid)
        os.chmod(p, mode)
    for row in plan['files']:
        p = guest(root, row['path'])
        require(p.parent.stat().st_dev == root.stat().st_dev, 'File outside image')
        if not row['already_exact']:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, row['mode'])
            try:
                data = base64.b64decode(row['data_b64'])
                with os.fdopen(fd, 'wb', closefd=False) as f:
                    f.write(data)
                    f.flush()
                os.fchown(fd, row['uid'], row['gid'])
                os.fchmod(fd, row['mode'])
                os.fsync(fd)
            finally:
                os.close(fd)
        else:
            os.chown(p, row['uid'], row['gid'])
            os.chmod(p, row['mode'])
    return verify(root, plan)


def verify(root, plan):
    root = Path(root).resolve()
    require(str(root) == plan['root'], 'Another root supplied')
    for name, (uid, gid, mode) in DIRECTORIES.items():
        p = guest(root, name)
        s = p.stat()
        require(p.is_dir() and (s.st_uid, s.st_gid, stat.S_IMODE(s.st_mode)) == (uid, gid, mode),
                'Directory metadata mismatch: ' + name)
    for row in plan['files']:
        p = guest(root, row['path'])
        s = p.stat()
        require((s.st_uid, s.st_gid, stat.S_IMODE(s.st_mode)) == (row['uid'], row['gid'], row['mode'])
                and hashlib.sha256(p.read_bytes()).hexdigest() == row['sha256'],
                'Seed readback mismatch: ' + row['path'])
    return {'passed': True, 'files': len(plan['files']), 'directories': len(DIRECTORIES),
            'device_key_material_copied': False, 'hardware_security_implemented': False}
