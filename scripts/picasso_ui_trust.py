"""Qualify one locally built UIKit library beside unchanged original trust modules."""
import hashlib
from pathlib import Path
import re
import struct
import uuid

from picasso_target import TRUST_BUNDLE_SHA256


def library_identity(data):
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise ValueError('Require a thin arm64 Mach-O library')
    _, cpu, _, kind, commands, command_bytes, _, _ = struct.unpack_from('<8I', data)
    if cpu != 0x100000c or kind != 6 or 32 + command_bytes > len(data):
        raise ValueError('Require an arm64 dynamic library')
    cursor, signatures = 32, []
    for _ in range(commands):
        if cursor + 8 > 32 + command_bytes:
            raise ValueError('Truncated load command')
        command, size = struct.unpack_from('<2I', data, cursor)
        if size < 8 or cursor + size > 32 + command_bytes:
            raise ValueError('Invalid load command size')
        if command == 0x1d:
            if size != 16:
                raise ValueError('Invalid signature command')
            signatures.append(struct.unpack_from('<2I', data, cursor + 8))
        cursor += size
    if cursor != 32 + command_bytes or len(signatures) != 1:
        raise ValueError('Require one code signature')
    offset, length = signatures[0]
    if offset < cursor or length < 12 or offset + length != len(data):
        raise ValueError('Invalid signature extent')
    signature = data[offset:]
    magic, extent, count = struct.unpack_from('>3I', signature)
    if magic != 0xfade0cc0 or not 12 + count * 8 <= extent <= length:
        raise ValueError('Invalid signature superblob')
    directories = []
    for index in range(count):
        slot, start = struct.unpack_from('>2I', signature, 12 + index * 8)
        if start < 12 + count * 8 or start + 8 > extent:
            raise ValueError('Invalid signature blob offset')
        blob_magic, size = struct.unpack_from('>2I', signature, start)
        if size < 8 or start + size > extent:
            raise ValueError('Invalid signature blob size')
        if slot == 0:
            if blob_magic != 0xfade0c02 or size < 44:
                raise ValueError('Invalid primary CodeDirectory')
            directories.append(signature[start:start + size])
    if len(directories) != 1:
        raise ValueError('Require one primary CodeDirectory')
    directory = directories[0]
    _, _, version, flags, hash_offset, identifier, special, slots, limit = struct.unpack_from('>9I', directory)
    hash_size, hash_type, _, page_shift = directory[36:40]
    # Native arm64 codesign uses 16 KiB slots; older toolchains use 4 KiB.
    if not 0x20001 <= version <= 0x20600 or not flags & 2 or hash_size != 32 or hash_type != 2 or page_shift not in (12, 14):
        raise ValueError('Require an ad-hoc SHA256 signature with 4 or 16 KiB code slots')
    page_size = 1 << page_shift
    if limit != offset or slots != (limit + page_size - 1) // page_size or not 44 <= identifier < len(directory):
        raise ValueError('Invalid signed code extent')
    if b'\0' not in directory[identifier:] or hash_offset < special * 32 or hash_offset + slots * 32 > len(directory):
        raise ValueError('Invalid CodeDirectory layout')
    for slot in range(slots):
        page = data[slot * page_size:min((slot + 1) * page_size, limit)]
        if hashlib.sha256(page).digest() != directory[hash_offset + slot * 32:hash_offset + (slot + 1) * 32]:
            raise ValueError('Library code page differs from its signature')
    return dict(sha256=hashlib.sha256(data).hexdigest(), cdhash=hashlib.sha256(directory).digest()[:20].hex(),
                code_page_bytes=page_size, code_slots=slots)


def original_modules(original):
    if hashlib.sha256(original).hexdigest() != TRUST_BUNDLE_SHA256:
        raise ValueError('Original trust bundle differs')
    count, first, second = struct.unpack_from('<3I', original)
    if count != 2 or first != 12 or not first < second < len(original):
        raise ValueError('Invalid original module offsets')
    return [original[first:second], original[second:]]


def owned_module(cdhash_hex, namespace='picasso:statusbar:'):
    cdhash = bytes.fromhex(cdhash_hex)
    if len(cdhash) != 20:
        raise ValueError('Require a 20-byte CDHash')
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, namespace + cdhash_hex)
    return struct.pack('<I16sI', 2, identifier.bytes, 1) + cdhash + bytes([2, 0, 0, 0])


def compat_modules(compat_cdhashes, library_cdhash):
    """One module per already-installed library CDHash.

    The persistent Data seed carries the inject library that was written when the
    volume was set up; rebuilding the library does not update that copy. A trust
    bundle that only lists the freshly built CDHash therefore rejects the very
    file the device is running (AMFI "unsuitable CT policy"). Keeping the
    installed hashes here makes the image independent of which seed the
    assemble step has to reuse.
    """
    modules, seen = [], {library_cdhash.lower()}
    for cdhash in compat_cdhashes:
        value = cdhash.strip().lower()
        if not re.fullmatch('[0-9a-f]{40}', value):
            raise ValueError('Compat CDHash must be 40 lowercase hex characters: %r' % cdhash)
        if value in seen:
            continue  # The fresh library already covers it, or it was repeated.
        seen.add(value)
        modules.append(owned_module(value, 'picasso:statusbar-compat:'))
    return modules


def make_bundle(original, library, compat_cdhashes=()):
    modules = original_modules(original)
    identity = library_identity(library)
    modules.append(owned_module(identity['cdhash']))
    extra = compat_modules(compat_cdhashes, identity['cdhash'])
    modules.extend(extra)
    cursor, offsets = 4 * (1 + len(modules)), []
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    bundle = struct.pack('<%dI' % (1 + len(modules)), len(modules), *offsets) + b''.join(modules)
    return bundle, dict(library=identity, original_bundle_sha256=TRUST_BUNDLE_SHA256,
                        original_modules_unchanged=True, owned_entries=1 + len(extra),
                        compat_cdhashes=[m[24:44].hex() for m in extra],
                        bundle_sha256=hashlib.sha256(bundle).hexdigest())


def verify_bundle(bundle, library, compat_cdhashes=()):
    if len(bundle) < 16:
        raise ValueError('Truncated UI trust bundle')
    count = struct.unpack_from('<I', bundle)[0]
    if count < 3 or count > 64:
        raise ValueError('Require at least two original modules and one UI module')
    header = 4 * (1 + count)
    offsets = struct.unpack_from('<%dI' % count, bundle, 4)
    if offsets[0] != header or list(offsets) != sorted(offsets) or offsets[-1] >= len(bundle):
        raise ValueError('Invalid module offsets')
    bounds = list(offsets) + [len(bundle)]
    if any(bounds[i] >= bounds[i + 1] for i in range(len(bounds) - 1)):
        raise ValueError('Empty trust module')
    original = struct.pack('<3I', 2, 12, 12 + bounds[1] - offsets[0]) + bundle[offsets[0]:bounds[2]]
    expected, report = make_bundle(original, library, compat_cdhashes)
    if bundle != expected:
        raise ValueError('UI trust module differs from the exact signed library')
    return report


def load_compat_cdhashes(path):
    """Read the trust-compat config; absent file means no extra hashes."""
    if path is None:
        return []
    import json
    document = json.loads(Path(path).read_text(encoding='utf-8'))
    if document.get('schema') != 'picasso-trust-compat-v1':
        raise ValueError('Unsupported trust-compat schema: %r' % document.get('schema'))
    return list(document.get('cdhashes', []))


def accepted_payload_bundle(bundle, library_path, device, compat_cdhashes=()):
    device = Path(device).resolve()
    library = (device / library_path).resolve()
    if library_path != 'out/picasso/libpicasso-sb.dylib' or not library.is_relative_to(device):
        raise ValueError('Unsupported owned trust library')
    return verify_bundle(bundle, library.read_bytes(), compat_cdhashes)
