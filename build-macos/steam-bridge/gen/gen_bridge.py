#!/usr/bin/env python3
"""Generates the Steam bridge's interface code from the Steamworks SDK headers.

For every Steamworks interface version it emits two halves that share one wire layout:

  mac/<Class>_<Version>.cpp   a class deriving from that SDK's interface class, each
                              method marshalling its arguments and calling the bridge
                              (compiled by clang into steamclient.dylib; the compiler
                              supplies the game-facing Itanium ABI)
  win/<Class>_<Version>.cpp   the matching handler: unmarshal, call the Windows object's
                              vtable slot (MSVC order, MSVC record returns), marshal the
                              results (compiled by mingw into sevo-steambridge.exe)

plus the interface and method tables, the callback size/conversion table, and
REPORT.md listing every decision a human should check. The SDK tables (versions,
sources, aliases, path-converting methods) are adapted from Proton's
lsteamclient/gen_wrapper.py (Valve, BSD-3-Clause).

    gen_bridge.py --sdk-root DIR --out DIR [--only SteamUser021,...] [--cache FILE]

The SDK root holds one steamworks_sdk_<ver> directory per version (build-macos/
steam-bridge/fetch-sdk.sh assembles it). Parsing needs libclang (`pip install
libclang`), Xcode's clang for the macOS target and mingw-w64's headers for the
Windows target.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys

# --- SDK tables, after Proton -------------------------------------------------------

# Newest first. "153" is the initial 1.53 SDK, which Proton skipped; its networking
# headers are the GameNetworkingSockets copies of that release (fetch-sdk.sh).
SDK_VERSIONS = [
    "165", "164", "163", "162", "161", "160", "159", "158", "157", "156", "155", "154",
    "153a", "153", "152", "151", "150", "149", "148a", "147", "146", "145", "144", "143y",
    "143x", "143", "142", "141", "140", "139", "138a", "138", "137", "136", "135a", "135",
    "134", "133x", "133b", "133a", "133", "132x", "132", "131", "130x", "130", "129a",
    "129", "128x", "128", "127", "126a", "126", "125", "124", "123a", "123", "122",
    "121x", "121", "120", "119x", "119", "118", "117", "116x", "116", "115", "114", "113",
    "112x", "112", "111x", "111", "110", "109", "108", "107", "106", "105", "104", "103",
    "102x", "102", "101x", "101", "100", "099y", "099x", "099w", "099v", "099u",
]

SDK_SOURCES = {
    "steam_api.h": [
        "ISteamApps", "ISteamAppList", "ISteamClient", "ISteamController",
        "ISteamGameSearch", "ISteamFriends", "ISteamHTMLSurface", "ISteamHTTP",
        "ISteamInput", "ISteamInventory", "ISteamMatchmaking", "ISteamMatchmakingServers",
        "ISteamMusic", "ISteamMusicRemote", "ISteamNetworking", "ISteamParties",
        "ISteamRemotePlay", "ISteamRemoteStorage", "ISteamScreenshots", "ISteamUGC",
        "ISteamUnifiedMessages", "ISteamUser", "ISteamUserStats", "ISteamUtils",
        "ISteamVideo",
    ],
    "isteambilling.h": ["ISteamBilling"],
    "isteamappticket.h": ["ISteamAppTicket"],
    "isteamgameserver.h": ["ISteamGameServer"],
    "isteamgameserverstats.h": ["ISteamGameServerStats"],
    "isteamgamestats.h": ["ISteamGameStats"],
    "isteammasterserverupdater.h": ["ISteamMasterServerUpdater"],
    "isteamgamecoordinator.h": ["ISteamGameCoordinator"],
    "isteamparentalsettings.h": ["ISteamParentalSettings"],
    "isteamnetworkingmessages.h": ["ISteamNetworkingMessages"],
    "isteamnetworkingsockets.h": ["ISteamNetworkingSockets"],
    "isteamnetworkingsocketsserialized.h": ["ISteamNetworkingSocketsSerialized"],
    "isteamnetworkingutils.h": ["ISteamNetworkingUtils"],
    "steamnetworkingfakeip.h": ["ISteamNetworkingFakeUDPPort"],
    "isteamtimeline.h": ["ISteamTimeline"],
}

SDK_CLASSES = {klass: source for source, klasses in SDK_SOURCES.items() for klass in klasses}

# Undocumented, binary-compatible interface versions: "served version": [names].
VERSION_ALIASES = {
    "SteamUtils004": ["SteamUtils003"],
    "SteamUtils002": ["SteamUtils001"],
    "SteamGameServer008": ["SteamGameServer007", "SteamGameServer006"],
    "SteamNetworkingSocketsSerialized002": ["SteamNetworkingSocketsSerialized001"],
    "STEAMAPPS_INTERFACE_VERSION001": ["SteamApps001"],
    "SteamNetworkingSockets002": ["SteamNetworkingSockets003"],
}

# Methods whose out-buffer carries a Windows path the game must see as a macOS path.
PATH_CONV_METHODS_UTOW = {
    "ISteamAppList_GetAppInstallDir": {"pchDirectory": "cchNameMax", "ret_size": True},
    "ISteamApps_GetAppInstallDir": {"pchFolder": "cchFolderBufferSize", "ret_size": True},
    "ISteamUGC_GetItemInstallInfo": {"pchFolder": "cchFolderSize"},
    "ISteamUser_GetUserDataFolder": {"pchBuffer": "cubBuffer"},
}

# Methods returning a Windows path as their string result.
PATH_CONV_RETURN = {
    "ISteamInput_GetGlyphForActionOrigin",
    "ISteamInput_GetGlyphPNGForActionOrigin",
    "ISteamInput_GetGlyphSVGForActionOrigin",
    "ISteamInput_GetGlyphForActionOrigin_Legacy",
    "ISteamInput_GetGlyphForXboxOrigin",
    "ISteamController_GetGlyphForActionOrigin",
    "ISteamController_GetGlyphForXboxOrigin",
}

# Methods taking a macOS file path the Windows client must see as a Windows path.
PATH_CONV_METHODS_WTOU = {
    "ISteamApps_GetFileDetails": ["pszFileName"],
    "ISteamHTMLSurface_FileLoadDialogResponse": ["pchSelectedFiles"],
    "ISteamRemoteStorage_PublishWorkshopFile": ["pchFile", "pchPreviewFile"],
    "ISteamRemoteStorage_UpdatePublishedFileFile": ["pchFile"],
    "ISteamRemoteStorage_UpdatePublishedFilePreviewFile": ["pchPreviewFile"],
    "ISteamRemoteStorage_PublishVideo": ["pchPreviewFile"],
    "ISteamScreenshots_AddScreenshotToLibrary": ["pchFilename", "pchThumbnailFilename"],
    "ISteamScreenshots_AddVRScreenshotToLibrary": ["pchFilename", "pchVRFilename"],
    "ISteamRemoteStorage_UGCDownloadToLocation": ["pchLocation"],
    "ISteamUGC_SetItemContent": ["pszContentFolder"],
    "ISteamUGC_SetItemPreview": ["pszPreviewFile"],
    "ISteamUGC_AddItemPreviewFile": ["pszPreviewFile"],
    "ISteamUGC_UpdateItemPreviewFile": ["pszPreviewFile"],
    "ISteamUGC_BInitWorkshopForGameServer": ["pszFolder"],
    "ISteamUtils_CheckFileSignature": ["szFileName"],
    "ISteamController_Init": ["pchAbsolutePathToControllerConfigVDF"],
    "ISteamInput_SetInputActionManifestFilePath": ["pchInputActionManifestAbsolutePath"],
}

# Interface pointers a method returns without a pchVersion argument to name them.
FIXED_RETURN_VERSIONS = {
    "CreateFakeUDPPort": "SteamNetworkingFakeUDPPort001",
}

# Pointer parameters the naming rules get wrong: "Class_Method": {"param": decision},
# where a decision is "single", "fixed:<n>" (an array of n elements the callee fills, as
# the SDK's STEAM_OUT_ARRAY_COUNT(<constant>) says), "string", "count:<param>"
# (elements), "bytes:<param>" or "unsupported". A count parameter that is itself a
# pointer is read through (`*punCount`).
_INPUT_ARRAYS = {
    "GetConnectedControllers": {"handlesOut": "fixed:16"},   # STEAM_{INPUT,CONTROLLER}_MAX_COUNT
    "GetDigitalActionOrigins": {"originsOut": "fixed:8"},    # STEAM_{INPUT,CONTROLLER}_MAX_ORIGINS
    "GetAnalogActionOrigins": {"originsOut": "fixed:8"},
    "GetActiveActionSetLayers": {"handlesOut": "fixed:16"},  # STEAM_{INPUT,CONTROLLER}_MAX_ACTIVE_LAYERS
}
PARAM_OVERRIDES = {
    **{f"{klass}_{method}": decisions for klass in ("ISteamInput", "ISteamController")
       for method, decisions in _INPUT_ARRAYS.items()},
    "ISteamInventory_GetResultItems": {"pOutItemsArray": "count:punOutItemsArraySize"},
    "ISteamInventory_GetItemDefinitionIDs": {"pItemDefIDs": "count:punItemDefIDsArraySize"},
    "ISteamInventory_GetEligiblePromoItemDefinitionIDs": {"pItemDefIDs": "count:punItemDefIDsArraySize"},
    "ISteamInventory_DeserializeResult": {"pOutResultHandle": "single"},
    "ISteamNetworkingUtils_GetPOPList": {"list": "count:nListSz"},
    "ISteamNetworkingSockets_GetConnectionRealTimeStatus": {"pLanes": "count:nLanes"},
    "ISteamMatchmaking_RequestLobbyList": {"pFilters": "count:nFilters"},
    "ISteamHTTP_GetHTTPRequestWasTimedOut": {"pbWasTimedOut": "single"},
    "ISteamHTTP_GetHTTPDownloadProgressPct": {"pflPercentOut": "single"},
    "ISteamInventory_ExchangeItems": {"pResultHandle": "single"},
    "ISteamInventory_GenerateItems": {"pResultHandle": "single"},
    "ISteamInventory_GetItemsByID": {"pResultHandle": "single"},
    "ISteamInventory_AddPromoItems": {"pResultHandle": "single"},
    "ISteamInventory_TransferItemQuantity": {"pResultHandle": "single"},
    "ISteamInventory_ConsumeItem": {"pResultHandle": "single"},
    "ISteamInventory_TradeItems": {"pResultHandle": "single"},
    "ISteamInventory_TriggerItemDrop": {"pResultHandle": "single"},
    "ISteamInventory_GrantPromoItems": {"pResultHandle": "single"},
    "ISteamInventory_AddPromoItem": {"pResultHandle": "single"},
    "ISteamInventory_GetAllItems": {"pResultHandle": "single"},
}

DEFINE_INTERFACE_VERSION = re.compile(r'^#define\s*(?P<name>STEAM(?:\w*)_VERSION(?:\w*))\s*"(?P<version>.*)"')

# --- parsing ---------------------------------------------------------------------------

MINGW_TRIPLE = "x86_64-w64-mingw32"
MINGW_INCLUDE_FALLBACK = f"/opt/homebrew/opt/mingw-w64/toolchain-x86_64/{MINGW_TRIPLE}/include"


def mingw_include():
    """mingw-w64's Windows headers: $MINGW_INCLUDE, else under the cross gcc's sysroot,
    else Homebrew's location."""
    configured = os.environ.get("MINGW_INCLUDE")
    if configured:
        return configured
    try:
        sysroot = subprocess.check_output([f"{MINGW_TRIPLE}-gcc", "-print-sysroot"], text=True,
                                          stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        sysroot = ""
    candidate = os.path.join(sysroot, MINGW_TRIPLE, "include") if sysroot else ""
    if candidate and os.path.isfile(os.path.join(candidate, "windows.h")):
        return candidate
    return MINGW_INCLUDE_FALLBACK


TARGETS = {
    "win": "x86_64-w64-windows-gnu",
    "mac": "arm64-apple-macos11",
    "mac_x86": "x86_64-apple-macos11",
}


def clang_args(target, sdk_root):
    resource_dir = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
    args = ["-x", "c++", "-std=c++14", f"--target={TARGETS[target]}", "-I" + sdk_root,
            "-isystem", os.path.join(resource_dir, "include"), "-Wno-everything"]
    if target == "win":
        args += ["-fms-extensions", "-isystem", mingw_include()]
    else:
        sysroot = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        args += ["-isysroot", sysroot]
    return args


def sdk_versions_and_includes(sdkdir):
    """The interface versions an SDK defines, and the SDK_SOURCES files it can include."""
    versions = {}
    dead = set()
    for name in os.listdir(sdkdir):
        path = os.path.join(sdkdir, name)
        if not os.path.isfile(path):
            continue
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
        if """#error "This file isn't used any more"\n""" in lines:
            dead.add(name)
        for line in lines:
            m = DEFINE_INTERFACE_VERSION.match(line)
            if not m:
                continue
            key = m["name"].replace("_INTERFACE_VERSION", "").replace("_VERSION", "")
            versions[key] = m["version"]
    includes = [f for f in SDK_SOURCES if f not in dead and os.path.exists(os.path.join(sdkdir, f))]
    return versions, includes


def parse_sdk(sdk_root, sdkver, target):
    """One SDK on one target, reduced to plain data (runs in a worker process)."""
    from clang.cindex import CursorKind, Index, TypeKind

    INTS = {TypeKind.CHAR_S, TypeKind.SCHAR, TypeKind.SHORT, TypeKind.INT, TypeKind.LONG,
            TypeKind.LONGLONG, TypeKind.INT128}
    UINTS = {TypeKind.CHAR_U, TypeKind.UCHAR, TypeKind.USHORT, TypeKind.UINT, TypeKind.ULONG,
             TypeKind.ULONGLONG, TypeKind.UINT128, TypeKind.CHAR16, TypeKind.CHAR32,
             TypeKind.WCHAR}

    def is_class_name(name):
        return name in SDK_CLASSES or name.startswith("ISteam")

    def describe_type(t):
        d = {"spell": t.spelling}
        c = t.get_canonical()
        k = c.kind
        if k == TypeKind.VOID:
            d["k"] = "void"
        elif k == TypeKind.BOOL:
            d["k"], d["size"] = "bool", 1
        elif k in INTS:
            d["k"], d["size"] = "int", c.get_size()
        elif k in UINTS:
            d["k"], d["size"] = "uint", c.get_size()
        elif k == TypeKind.FLOAT:
            d["k"], d["size"] = "float", 4
        elif k == TypeKind.DOUBLE:
            d["k"], d["size"] = "double", 8
        elif k == TypeKind.ENUM:
            d["k"], d["size"], d["name"] = "enum", c.get_size(), c.spelling.removeprefix("const ")
        elif k == TypeKind.RECORD:
            name = c.spelling.removeprefix("const ")
            if is_class_name(name):
                d["k"], d["name"] = "class", name
            else:
                d["k"], d["name"], d["size"], d["align"] = "record", name, c.get_size(), c.get_align()
                decl = c.get_declaration()
                if decl.semantic_parent is not None and decl.semantic_parent.kind in (
                        CursorKind.STRUCT_DECL, CursorKind.UNION_DECL, CursorKind.CLASS_DECL) \
                        or "(" in name:
                    d["record"] = describe_record(decl)
        elif k in (TypeKind.POINTER, TypeKind.LVALUEREFERENCE, TypeKind.RVALUEREFERENCE):
            pt = c.get_pointee()
            if pt.get_canonical().kind == TypeKind.FUNCTIONPROTO:
                d["k"] = "funcptr"
            else:
                d["k"] = "ptr" if k == TypeKind.POINTER else "ref"
                d["pointee"] = describe_type(pt)
                d["const"] = pt.is_const_qualified()
        elif k == TypeKind.CONSTANTARRAY:
            d["k"], d["count"], d["elem"] = "array", c.element_count, describe_type(c.element_type)
        else:
            d["k"], d["kind"] = "other", str(k)
        return d

    def describe_record(decl):
        t = decl.type.get_canonical()
        rec = {"size": t.get_size(), "align": t.get_align(),
               "union": decl.kind == CursorKind.UNION_DECL, "fields": [], "bitfields": False,
               "callback_id": None}
        for ch in decl.get_children():
            if ch.kind == CursorKind.ENUM_DECL:
                for e in ch.get_children():
                    if e.displayname == "k_iCallback":
                        rec["callback_id"] = int(e.enum_value)
        if rec["union"]:
            return rec

        def add_fields(cursor):
            for f in cursor.type.get_fields():
                if f.is_bitfield():
                    rec["bitfields"] = True
                field = {"name": f.spelling, "off": f.get_field_offsetof() // 8,
                         "type": describe_type(f.type)}
                ft = field["type"]
                inner = ft["elem"] if ft["k"] == "array" else ft
                if inner["k"] == "record" and "record" not in inner:
                    inner_decl = f.type.get_canonical()
                    if ft["k"] == "array":
                        inner_decl = inner_decl.element_type.get_canonical()
                    inner["record"] = describe_record(inner_decl.get_declaration())
                rec["fields"].append(field)

        for ch in decl.get_children():
            if ch.kind == CursorKind.CXX_BASE_SPECIFIER:
                base = ch.type.get_declaration()
                if len(list(base.type.get_fields())) > 0:
                    add_fields(base)
                break
        add_fields(decl)
        return rec

    def describe_class(c, versions):
        key = c.spelling[1:].upper()
        version = versions.get(key)
        if not version:
            return None
        methods = []
        overrides = {}
        index = 0
        for m in c.get_children():
            is_virtual = m.kind == CursorKind.CXX_METHOD and m.is_virtual_method()
            if not is_virtual and m.kind != CursorKind.DESTRUCTOR:
                continue
            first, override = overrides.get(m.spelling, (index, 1))
            overrides[m.spelling] = (first, override + 1)
            params = []
            for i, p in enumerate(m.get_arguments()):
                params.append({"name": p.spelling or f"_{chr(0x61 + i)}", "type": describe_type(p.type)})
            methods.append({
                "name": m.spelling if override == 1 else f"{m.spelling}_{override}",
                "spelling": m.spelling,
                "dtor": m.kind == CursorKind.DESTRUCTOR,
                "index": first,
                "override": override,
                "const": m.is_const_method(),
                "result": describe_type(m.result_type),
                "params": params,
            })
            index += 1
        return {"name": c.spelling, "version": version, "methods": methods}

    sdkdir = os.path.join(sdk_root, f"steamworks_sdk_{sdkver}")
    versions, includes = sdk_versions_and_includes(sdkdir)
    source = "\n".join(f'#include "steamworks_sdk_{sdkver}/{f}"' for f in includes) + "\n"
    tu = Index.create().parse("source.cpp", args=clang_args(target, sdk_root),
                              unsaved_files=[("source.cpp", source)])
    errors = [str(d) for d in tu.diagnostics if d.severity >= 3]

    classes = {}
    records = {}
    for c in tu.cursor.get_children():
        if not c.is_definition() or c.kind == CursorKind.TYPEDEF_DECL:
            continue
        if c.type.get_canonical().kind != TypeKind.RECORD:
            continue
        name = c.spelling
        if not name or "(" in name:
            continue
        if c.kind == CursorKind.CLASS_DECL and name in SDK_CLASSES:
            klass = describe_class(c, versions)
            if klass:
                classes[klass["version"]] = klass
        elif name not in records:
            records[name] = describe_record(c)
    return {"sdkver": sdkver, "target": target, "errors": errors, "versions": versions,
            "includes": includes, "classes": classes, "records": records}


def parse_all(sdk_root, sdkvers, jobs):
    tasks = [(sdkver, target) for sdkver in sdkvers for target in TARGETS]
    results = {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(parse_sdk, sdk_root, s, t): (s, t) for s, t in tasks}
        done = 0
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            done += 1
            print(f"parsed {r['sdkver']}/{r['target']} ({done}/{len(tasks)})", file=sys.stderr)
            if r["errors"]:
                sys.exit(f"SDK {r['sdkver']} on {r['target']}: " + "\n".join(r["errors"][:5]))
            results.setdefault(r["sdkver"], {})[r["target"]] = r
    return results


# --- layouts and conversion runs ---------------------------------------------------------

def leaves(rec, base, out):
    """(offset, size) of every scalar in a record, in declaration order; a union, a record
    with bitfields or an unknown nested record is one opaque leaf."""
    if rec["union"] or rec["bitfields"]:
        out.append((base, rec["size"]))
        return
    for f in rec["fields"]:
        add_leaf(f["type"], base + f["off"], out)


def add_leaf(t, off, out):
    k = t["k"]
    if k == "record":
        if "record" in t:
            leaves(t["record"], off, out)
        else:
            out.append((off, t["size"]))
    elif k == "array":
        elem = t["elem"]
        if elem["k"] == "record" and "record" in elem:
            for i in range(t["count"]):
                leaves(elem["record"], off + i * elem["size"], out)
        else:
            out.append((off, t["count"] * type_size(elem)))
    else:
        out.append((off, type_size(t)))


def type_size(t):
    k = t["k"]
    if k in ("ptr", "ref", "funcptr"):
        return 8
    if k == "array":
        return t["count"] * type_size(t["elem"])
    if k == "class":
        return 8
    return t.get("size", 8)


def has_pointers(t):
    """Whether a record (or a type reaching one) holds pointers the other side cannot use."""
    k = t["k"]
    if k in ("ptr", "ref", "funcptr", "class"):
        return True
    if k == "array":
        return has_pointers(t["elem"])
    if k == "record" and "record" in t:
        return record_has_pointers(t["record"])
    return False


def record_has_pointers(rec):
    if rec["union"] or rec["bitfields"]:
        return False
    return any(has_pointers(f["type"]) for f in rec["fields"])


def conversion_runs(mac_rec, win_rec):
    """Copy runs between the two layouts, or [] when they are identical."""
    m, w = [], []
    leaves(mac_rec, 0, m)
    leaves(win_rec, 0, w)
    if len(m) != len(w) or any(a[1] != b[1] for a, b in zip(m, w)):
        return None
    runs = []
    for (mo, ms), (wo, ws) in zip(m, w):
        if runs and runs[-1][0] + runs[-1][2] == wo and runs[-1][1] + runs[-1][2] == mo:
            runs[-1][2] += ms
        else:
            runs.append([wo, mo, ms])
    if len(runs) == 1 and runs[0][0] == 0 and runs[0][1] == 0 and mac_rec["size"] == win_rec["size"]:
        return []
    return runs


# --- the model: one interface version with classified methods ----------------------------

COUNT_PREFIX = re.compile(r"^(cch|cub|cb|c[A-Z]|k_)")
COUNT_WORD = re.compile(r"(Size|Count|Len|Max|Bytes|Num|Entries|Elements|Items)")
BYTES_HINT = re.compile(r"^(cub|cb|cch)|Size|Bytes")
ARRAY_PREFIX = re.compile(r"^(prg|pArray|pvec|pub|pv[A-Z]|pOut\w*Array|p\w*Prices|p\w*Quantit|pData|pDest|pBuf|pBlob|pch|psz|p\w*(Buffer|Array|Items|Defs|IDs|Result))")


def is_int(t):
    return t["k"] in ("int", "uint") and t.get("size", 0) <= 8


def is_count_name(name):
    return bool(COUNT_PREFIX.match(name) or COUNT_WORD.search(name))


def counts_bytes(name):
    return bool(BYTES_HINT.search(name))


class Param:
    """One parameter's wire kind and the code each side needs for it."""

    def __init__(self, name, t):
        self.name = name
        self.type = t
        self.kind = None          # scalar | record | string | array | outstr | funcptr | unsupported
        self.reason = None        # why unsupported
        self.count = None         # count parameter name (array kinds)
        self.count_ptr = False    # the count is *param
        self.count_bytes = False  # the count is in bytes, not elements
        self.fixed = None         # fixed element count (array references, fixed:<n>, singles)
        self.guessed_single = False  # a single only because nothing sized it
        self.inout = False        # non-const pointer: contents go both ways
        self.always = False       # a reference: never null
        self.elem = None          # element type (array kinds) / record type
        self.path_in = False      # a macOS path the client must see as Windows
        self.path_out = None      # the count parameter of an out-buffer carrying a Windows path


class MethodModel:
    def __init__(self, klass, method, sdkver):
        self.klass = klass
        self.m = method
        self.sdkver = sdkver
        self.name = method["name"]
        self.full = f"{klass['name']}_{klass['version']}_{self.name}"
        self.key = f"{klass['name']}_{method['spelling']}"
        self.params = []
        self.unsupported = None
        self.local = False
        self.special = None
        self.ret = None           # void | scalar | record | string | iface | unsupported
        self.slot = None
        self.notes = []


def classify_method(mm):
    m = mm.m
    params = [Param(p["name"], p["type"]) for p in m["params"]]
    mm.params = params
    overrides = PARAM_OVERRIDES.get(mm.key, {})
    for i, p in enumerate(params):
        t = p.type
        k = t["k"]
        if k in ("bool", "int", "uint", "float", "double", "enum"):
            p.kind = "scalar"
            continue
        if k == "funcptr":
            p.kind = "funcptr"
            continue
        if k == "record":
            p.kind, p.elem = "record", t
            continue
        if k == "class":
            p.kind, p.reason = "unsupported", f"{p.name}: interface object by value"
            continue
        if k not in ("ptr", "ref"):
            p.kind, p.reason = "unsupported", f"{p.name}: {t['spell']}"
            continue
        pointee = t["pointee"]
        pk = pointee["k"]
        if pk == "class":
            p.kind, p.reason = "unsupported", f"{p.name}: game-side listener {pointee['name']}"
            continue
        if pk in ("ptr", "ref"):
            inner = pointee["pointee"]
            if inner["k"] in ("int", "uint") and inner["size"] == 1 and k == "ptr" and not t["const"]:
                p.kind = "outstr"
                continue
            p.kind, p.reason = "unsupported", f"{p.name}: pointer to pointer {t['spell']}"
            continue
        if pk == "funcptr":
            p.kind, p.reason = "unsupported", f"{p.name}: pointer to function pointer"
            continue
        if pk == "array":
            p.kind, p.elem, p.fixed = "array", pointee["elem"], pointee["count"]
            p.inout, p.always = not t["const"], k == "ref"
            continue
        override = overrides.get(p.name)
        is_char = pk in ("int", "uint") and pointee["size"] == 1 and \
            pointee["spell"].removeprefix("const ") in ("char", "unsigned char", "uint8", "int8", "uint8_t", "int8_t")
        is_bytes_type = pk == "void" or is_char
        if override == "string" or (override is None and t["const"] and pk != "void" and
                                     pointee["spell"].removeprefix("const ") == "char"):
            p.kind = "string"
            p.path_in = p.name in PATH_CONV_METHODS_WTOU.get(mm.key, [])
            continue
        if override == "unsupported":
            p.kind, p.reason = "unsupported", f"{p.name}: by override"
            continue
        p.elem = pointee
        p.inout, p.always = not t["const"], k == "ref"
        p.kind = "array"
        if override == "single":
            p.fixed = 1
            continue
        if override and override.startswith("fixed:"):
            p.fixed = int(override.split(":", 1)[1])
            continue
        if override and override.startswith(("count:", "bytes:")):
            p.count = override.split(":", 1)[1]
            p.count_bytes = override.startswith("bytes:")
            named = next((q for q in params if q.name == p.count), None)
            if named is None:
                raise GenError(f"{mm.full}: override names no parameter {p.count}")
            p.count_ptr = named.type["k"] == "ptr"
            continue
        count = find_count(params, i)
        if count is not None:
            q, via_ptr = count
            p.count, p.count_ptr, p.count_bytes = q.name, via_ptr, counts_bytes(q.name)
            continue
        if is_bytes_type:
            p.kind, p.reason = "unsupported", f"{p.name}: {t['spell']} with no size"
            continue
        p.fixed = 1
        p.guessed_single = True

    if mm.m["spelling"] == "GetAPICallResult" and mm.klass["name"] == "ISteamUtils":
        mm.special = "apicallresult"
    for p in params:
        if p.kind == "array" and p.elem["k"] == "record":
            if p.elem.get("size", 0) == 0:
                p.kind, p.reason = "unsupported", f"{p.name}: incomplete {p.elem['name']}"
        if p.kind in ("record", "array") and p.elem["k"] == "record" and has_pointers_named(p.elem):
            p.kind, p.reason = "unsupported", f"{p.name}: {p.elem['name']} holds pointers"
        if p.kind == "array" and p.elem["k"] in ("class", "ptr", "ref", "funcptr", "other", "void") and p.elem["k"] != "void":
            p.kind, p.reason = "unsupported", f"{p.name}: array of {p.elem['spell']}"
        if p.kind == "array" and p.elem["k"] == "void":
            p.elem = {"k": "uint", "size": 1, "spell": "uint8_t"}
    if any(p.kind == "funcptr" for p in params):
        mm.local = True

    r = m["result"]
    rk = r["k"]
    if rk == "void":
        mm.ret = "void"
    elif rk in ("bool", "int", "uint", "float", "double", "enum"):
        mm.ret = "scalar"
    elif rk == "record":
        mm.ret = "record" if not has_pointers_named(r) else "unsupported"
    elif rk == "ptr" and r["pointee"]["k"] == "class":
        mm.ret = "iface"
    elif rk == "ptr" and r["pointee"]["k"] == "void" and any(p.name == "pchVersion" for p in params):
        mm.ret = "iface"
    elif rk == "ptr" and r["const"] and r["pointee"]["k"] in ("int", "uint") and r["pointee"]["size"] == 1:
        mm.ret = "string"
    else:
        mm.ret = "unsupported"
    if mm.ret == "iface" and not any(p.name == "pchVersion" for p in params) \
            and mm.m["spelling"] not in FIXED_RETURN_VERSIONS:
        mm.ret = "unsupported"

    bad = [p.reason for p in params if p.kind == "unsupported"]
    if mm.ret == "unsupported":
        bad.append(f"returns {r['spell']}")
    if bad and not mm.local:
        mm.unsupported = "; ".join(bad)
    utow = PATH_CONV_METHODS_UTOW.get(mm.key)
    if utow:
        for p in params:
            if p.name in utow:
                p.path_out = utow[p.name]


def has_pointers_named(t):
    """Pointer check for a top-level record named in a method signature."""
    return t.get("_has_pointers", False)


def find_count(params, i):
    """The parameter sizing pointer params[i]: the count right after it, a count pointer
    right after it, or a count after a run of array-like pointers. None for a single."""
    p = params[i]
    if i + 1 >= len(params):
        return None
    nxt = params[i + 1]
    if is_int(nxt.type) and is_count_name(nxt.name):
        return nxt, False
    nt = nxt.type
    pointee = p.type["pointee"]
    bytes_type = pointee["k"] == "void" or (pointee["k"] in ("int", "uint") and pointee.get("size") == 1)
    if nt["k"] == "ptr" and not nt["const"] and is_int(nt["pointee"]) and nt["pointee"]["size"] >= 2 \
            and is_count_name(nxt.name) and nxt.name[0] == "p" and (bytes_type or ARRAY_PREFIX.match(p.name)):
        return nxt, True
    j = i + 1
    while j < len(params) and params[j].type["k"] == "ptr":
        j += 1
    if j < len(params) and j > i + 1 and is_int(params[j].type) and is_count_name(params[j].name) \
            and all(ARRAY_PREFIX.match(params[q].name) for q in range(i, j)):
        return params[j], False
    return None


ARRAY_LOOKING = re.compile(r"(Out|List|list|Array|array|Arr|Vec)$|^(prg|pvec|pArray)")
SINGULAR_S = re.compile(r"(ss|us|is|Details|Stats|Status|Flags|Bytes|Progress)$")


def looks_like_array(name):
    """Whether a pointer the rules took for one element is named like several: *Out,
    *List, *Array, prg*/pvec*, or a plural. Such a guess sized wrong overflows the
    helper's buffer, so each one needs a PARAM_OVERRIDES decision."""
    if is_count_name(name):
        return False
    if ARRAY_LOOKING.search(name):
        return True
    return name.endswith("s") and not SINGULAR_S.search(name)


def guessed_arrays(interfaces):
    """Every writable pointer guessed to be a single element whose name says otherwise."""
    found = set()
    for iface in interfaces:
        for mm in iface["methods"]:
            if mm.m["dtor"] or mm.unsupported or mm.local:
                continue
            for p in mm.params:
                if p.kind == "array" and p.guessed_single and p.inout and looks_like_array(p.name):
                    found.add(f"{mm.key} {p.name} ({p.type['spell']})")
    return sorted(found)


# --- emission helpers ------------------------------------------------------------------

SCALAR_C = {("bool", 1): "uint8_t", ("int", 1): "int8_t", ("int", 2): "int16_t", ("int", 4): "int32_t",
            ("int", 8): "int64_t", ("uint", 1): "uint8_t", ("uint", 2): "uint16_t", ("uint", 4): "uint32_t",
            ("uint", 8): "uint64_t", ("float", 4): "float", ("double", 8): "double"}
WIRE_FN = {("bool", 1): "u8", ("int", 1): "i8", ("int", 2): "i16", ("int", 4): "i32", ("int", 8): "i64",
           ("uint", 1): "u8", ("uint", 2): "u16", ("uint", 4): "u32", ("uint", 8): "u64",
           ("float", 4): "f32", ("double", 8): "f64"}


def scalar_key(t):
    k = t["k"]
    if k == "enum":
        return ("int", t["size"]) if t["size"] in (1, 2, 4, 8) else ("int", 4)
    return (k, t["size"])


def c_type(t):
    """The C type a scalar travels as, on either side."""
    return SCALAR_C[scalar_key(t)]


def wire_fn(t):
    return WIRE_FN[scalar_key(t)]


def mac_spell(t):
    return t["spell"]


class Emitter:
    def __init__(self, model, out_dir, parsed, sdk_rel):
        self.model = model
        self.out = out_dir
        self.parsed = parsed
        self.sdk_rel = sdk_rel
        self.run_tables = {}

    # Struct conversion runs for a record named in SDK `sdkver`; returns (wsize, msize,
    # runs-name-or-None, run-count). Reports records the leaves cannot pair.
    def record_info(self, sdkver, name):
        mac = self.parsed[sdkver]["mac"]["records"].get(name)
        win = self.parsed[sdkver]["win"]["records"].get(name)
        if mac is None or win is None:
            return None
        runs = conversion_runs(mac, win)
        if runs is None:
            return None
        return win["size"], mac["size"], runs

    def runs_decl(self, name, runs, sdkver):
        if not runs:
            return "nullptr", 0
        ident = f"runs_{sanitize(name)}_{sdkver}"
        body = ", ".join(f"{{{w}, {m}, {n}}}" for w, m, n in runs)
        self.run_tables[ident] = f"static const bridge::Run {ident}[] = {{ {body} }};\n"
        return ident, len(runs)


def sanitize(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


# --- the macOS proxy -------------------------------------------------------------------

def emit_mac_class(em, iface, sdkver, includes, method_ids):
    klass = iface["klass"]
    name, version = klass["name"], klass["version"]
    full = f"{name}_{version}"
    lines = [f"// Generated by gen_bridge.py from steamworks_sdk_{sdkver}; do not edit.",
             '#include "proxy_prelude.h"']
    for inc in includes:
        lines.append(f'#include "{em.sdk_rel}/steamworks_sdk_{sdkver}/{inc}"')
    lines.append("")
    lines.append("namespace {")
    lines.append(f"struct Proxy final : public {name} {{")
    lines.append("    uint64_t handle_;")
    lines.append("    explicit Proxy(uint64_t h) : handle_(h) {}")
    em.run_tables = {}
    bodies = []
    for mm in iface["methods"]:
        if mm.m["dtor"]:
            continue
        bodies.append(emit_mac_method(em, mm, sdkver, method_ids[mm.full]))
    for table in em.run_tables.values():
        lines.insert(len(lines) - 4, table)
    lines.extend(bodies)
    lines.append("};")
    lines.append("}  // namespace")
    lines.append("")
    lines.append(f"void *bridge_create_{full}(uint64_t handle) {{ return static_cast<{name} *>(new Proxy(handle)); }}")
    lines.append("")
    path = os.path.join(em.out, "mac", f"{full}.cpp")
    write_if_changed(path, "\n".join(lines))


def declarator(spell, name):
    """`type name`, with the name inside a function-pointer declarator."""
    if "(*)" in spell:
        return spell.replace("(*)", f"(*{name})", 1)
    return f"{spell} {name}"


def mac_signature(mm):
    r = mm.m["result"]["spell"]
    ps = ", ".join(declarator(p.type["spell"], p.name) for p in mm.params)
    const = " const" if mm.m["const"] else ""
    return f"    {r} {mm.m['spelling']}({ps}){const} override"


def mac_zero_return(mm):
    rk = mm.m["result"]["k"]
    if rk == "void":
        return "        return;"
    if mm.ret == "string":
        # Steam's own string getters never answer null, and games strlen the result.
        return '        return "";'
    if rk in ("ptr", "funcptr"):
        return "        return nullptr;"
    if rk == "record":
        return f"        {{ {mm.m['result']['spell']} zero; std::memset(&zero, 0, sizeof zero); return zero; }}"
    return f"        return ({mm.m['result']['spell']})0;"


def emit_mac_method(em, mm, sdkver, method_id):
    out = [mac_signature(mm) + " {"]
    if mm.local:
        out.append(f'        bridge::note_local("{mm.full}");')
        out.append(mac_zero_return(mm))
        out.append("    }")
        return "\n".join(out)
    if mm.unsupported:
        out.append(f'        bridge::note_unsupported("{mm.full}");')
        out.append(mac_zero_return(mm))
        out.append("    }")
        return "\n".join(out)
    if mm.special == "apicallresult":
        return emit_mac_apicallresult(mm, method_id)

    out.append(f"        bridge::Call c(handle_, {method_id});")
    post = []
    for p in mm.params:
        t = p.type
        if p.kind == "scalar":
            if t["k"] == "bool":
                out.append(f"        c.w.u8({p.name} ? 1 : 0);")
            else:
                out.append(f"        c.w.{wire_fn(t)}(({c_type(t)}){p.name});")
        elif p.kind == "string":
            if p.path_in:
                out.append(f"        c.w.str(bridge::path_to_win({p.name}).c_str());")
            else:
                out.append(f"        c.w.str({p.name});")
        elif p.kind == "record":
            info = em.record_info(sdkver, t["name"])
            if info is None:
                raise GenError(f"{mm.full}: no layout for {t['name']}")
            wsize, msize, runs = info
            ident, n = em.runs_decl(t["name"], runs, sdkver)
            out.append(f"        c.put_struct(&{p.name}, {msize}, {wsize}, {ident}, {n});")
        elif p.kind == "array":
            elem = p.elem
            if elem["k"] == "record":
                info = em.record_info(sdkver, elem["name"])
                if info is None:
                    raise GenError(f"{mm.full}: no layout for {elem['name']}")
                wsize, msize, runs = info
                ident, n = em.runs_decl(elem["name"], runs, sdkver)
            else:
                wsize = msize = type_size(elem)
                ident, n = "nullptr", 0
            count = array_count_expr(p, msize)
            ptr = f"&{p.name}" if p.always and p.type["k"] == "ref" else p.name
            if p.always and p.type["k"] == "ref" and p.type["pointee"]["k"] == "array":
                ptr = f"&{p.name}[0]"
            if p.inout:
                out.append(f"        c.put_inout({ptr}, {count}, {msize}, {wsize}, {ident}, {n});")
                post.append(f"        c.get_inout({ptr}, {count}, {msize}, {wsize}, {ident}, {n});")
            else:
                out.append(f"        c.put_in({ptr}, {count}, {msize}, {wsize}, {ident}, {n});")
        elif p.kind == "outstr":
            out.append(f"        c.w.u8({p.name} ? 1 : 0);")
            post.append(f"        if ({p.name}) *{p.name} = ({p.type['pointee']['spell']})c.keep_str(c.r.str());")
        else:
            raise GenError(f"{mm.full}: {p.name} unclassified")
    out.append("        if (!c.send()) {")
    out.append("    " + mac_zero_return(mm).strip())
    out.append("        }")
    r = mm.m["result"]
    if mm.ret == "void":
        out.extend(post)
    elif mm.ret == "scalar":
        if r["k"] == "bool":
            out.append("        bool ret = c.r.u8() != 0;")
        else:
            out.append(f"        {r['spell']} ret = ({r['spell']})c.r.{wire_fn(r)}();")
        out.extend(post)
        out.append("        return ret;")
    elif mm.ret == "record":
        info = em.record_info(sdkver, r["name"])
        if info is None:
            raise GenError(f"{mm.full}: no layout for returned {r['name']}")
        wsize, msize, runs = info
        ident, n = em.runs_decl(r["name"], runs, sdkver)
        out.append(f"        {r['spell']} ret; std::memset(&ret, 0, sizeof ret);")
        out.append(f"        c.get_struct(&ret, {msize}, {wsize}, {ident}, {n});")
        out.extend(post)
        out.append("        return ret;")
    elif mm.ret == "string":
        if mm.key in PATH_CONV_RETURN:
            out.append("        const char *ret = c.keep_str(bridge::path_to_mac(c.r.str()).c_str());")
        else:
            out.append("        const char *ret = c.keep_str(c.r.str());")
        out.extend(post)
        out.append("        return ret;")
    elif mm.ret == "iface":
        version = "pchVersion" if any(p.name == "pchVersion" for p in mm.params) \
            else f'"{FIXED_RETURN_VERSIONS[mm.m["spelling"]]}"'
        out.append("        uint64_t h = c.r.u64();")
        out.extend(post)
        out.append(f"        return ({r['spell']})bridge::proxy_for({version}, h);")
    for p in mm.params:
        if p.path_out:
            ret_size = PATH_CONV_METHODS_UTOW[mm.key].get("ret_size", False)
            # The buffer holds a Windows path; rewrite it in place as a macOS one. A path
            # too long for the game's buffer comes back empty, with the size it needs
            # (size-returning methods) or a false result.
            capacity = f"bridge::count({p.path_out})"
            convert = f"bridge::path_out_to_mac({p.name}, {capacity})"
            if ret_size:
                line = f"        ret = ret ? {convert} : 0;"
            elif mm.ret == "scalar" and r["k"] == "bool":
                line = f"        if ({convert} > {capacity}) ret = false;"
            else:
                raise GenError(f"{mm.full}: path out-buffer on a method returning {r['spell']}")
            out.insert(len(out) - (1 if mm.ret != "void" else 0), line)
    out.append("    }")
    return "\n".join(out)


def array_count_expr(p, msize):
    if p.fixed is not None:
        return str(p.fixed)
    src = f"({p.count} ? *{p.count} : 0)" if p.count_ptr else p.count
    if p.count_bytes and msize > 1:
        return f"(bridge::count({src}) / {msize})"
    return f"bridge::count({src})"


def emit_mac_apicallresult(mm, method_id):
    # bool GetAPICallResult(SteamAPICall_t, void *pCallback, int cubCallback, int iCallbackExpected, bool *pbFailed)
    names = [p.name for p in mm.params]
    return "\n".join([
        mac_signature(mm) + " {",
        f"        return bridge::api_call_result(handle_, {method_id}, {names[0]}, {names[1]}, {names[2]}, {names[3]}, {names[4]});",
        "    }",
    ])


# --- the Windows dispatcher -------------------------------------------------------------

def emit_win_class(em, iface, sdkver):
    klass = iface["klass"]
    full = f"{klass['name']}_{klass['version']}"
    lines = [f"// Generated by gen_bridge.py from steamworks_sdk_{sdkver}; do not edit.",
             '#include "dispatch_prelude.h"', "", "namespace {"]
    handlers = []
    for mm in iface["methods"]:
        if mm.m["dtor"] or mm.local or mm.unsupported:
            continue
        lines.append(emit_win_method(em, mm, sdkver))
        handlers.append(mm)
    lines.append("}  // namespace")
    lines.append("")
    for mm in handlers:
        lines.append(f"void bridge_h_{mm.full}(bridge::Req &q, bridge::Rep &p) {{ h_{mm.name}(q, p); }}")
    write_if_changed(os.path.join(em.out, "win", f"{full}.cpp"), "\n".join(lines) + "\n")


def emit_win_method(em, mm, sdkver):
    if mm.special == "apicallresult":
        return "\n".join([
            f"void h_{mm.name}(bridge::Req &q, bridge::Rep &p) {{",
            f"    bridge::api_call_result(q, p, {mm.slot});",
            "}",
        ])
    out = [f"void h_{mm.name}(bridge::Req &q, bridge::Rep &p) {{", "    void *obj = q.object();"]
    fn_params = ["void *"]
    args = ["obj"]
    post = []
    for p in mm.params:
        t = p.type
        v = f"a_{p.name}"
        if p.kind == "scalar":
            ct = c_type(t)
            out.append(f"    {ct} {v} = q.r.{wire_fn(t)}();")
            fn_params.append(ct)
            args.append(v)
        elif p.kind == "string":
            out.append(f"    const char *{v} = q.r.str();")
            fn_params.append("const char *")
            args.append(v)
        elif p.kind == "record":
            info = em.record_info(sdkver, t["name"])
            wsize = info[0]
            out.append(f"    bridge::Blob {v} = q.get_struct({wsize});")
            if wsize in (1, 2, 4, 8):
                ct = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[wsize]
                fn_params.append(ct)
                args.append(f"{v}.as<{ct}>()")
            else:
                fn_params.append("const void *")
                args.append(f"{v}.data")
        elif p.kind == "array":
            # A fixed-size array is allocated at its full Windows size whatever the
            # game sent, since the callee fills all of it.
            least = f"{p.fixed * win_elem_size(em, sdkver, p)}" if p.fixed is not None else ""
            if p.inout:
                out.append(f"    bridge::InOut {v} = q.get_inout({least});")
                post.append(f"    p.put_inout({v});")
                fn_params.append("void *")
                args.append(f"{v}.ptr()")
            else:
                out.append(f"    bridge::Blob {v} = q.get_in({least});")
                fn_params.append("const void *")
                args.append(f"{v}.data")
        elif p.kind == "outstr":
            out.append(f"    uint8_t has_{p.name} = q.r.u8(); const char *s_{p.name} = nullptr;")
            fn_params.append("const char **")
            args.append(f"has_{p.name} ? &s_{p.name} : nullptr")
            post.append(f"    if (has_{p.name}) p.w.str(s_{p.name});")
        else:
            raise GenError(f"{mm.full}: {p.name} unclassified")
    out.extend(win_count_limits(em, mm, sdkver))
    r = mm.m["result"]
    if mm.ret == "record":
        info = em.record_info(sdkver, r["name"])
        wsize = info[0]
        out.append(f"    bridge::Blob ret = bridge::Blob::zeroed({wsize});")
        fn_params.insert(1, "void *")
        args.insert(1, "ret.data")
        rt = "void *"
    elif mm.ret == "void":
        rt = "void"
    elif mm.ret == "scalar":
        rt = c_type(r)
    elif mm.ret == "string":
        rt = "const char *"
    elif mm.ret == "iface":
        rt = "void *"
    else:
        raise GenError(f"{mm.full}: unsupported return reached the emitter")
    out.append(f"    typedef {rt} (*Fn)({', '.join(fn_params)});")
    call = f"((Fn)bridge::slot(obj, {mm.slot}))({', '.join(args)})"
    if mm.ret == "void":
        out.append(f"    {call};")
    elif mm.ret == "record":
        out.append(f"    {call};")
        out.append("    p.w.bytes(ret.data, ret.size);")
    elif mm.ret == "scalar":
        out.append(f"    {rt} ret = {call};")
        out.append(f"    p.w.{wire_fn(r)}(ret);")
    elif mm.ret == "string":
        out.append(f"    const char *ret = {call};")
        out.append("    p.w.str(ret);")
    elif mm.ret == "iface":
        out.append(f"    void *ret = {call};")
        out.append("    p.w.u64((uint64_t)(uintptr_t)ret);")
    out.extend(post)
    out.append("}")
    return "\n".join(out)


def win_elem_size(em, sdkver, p):
    """The Windows size of one element of an array parameter."""
    if p.elem["k"] == "record":
        info = em.record_info(sdkver, p.elem["name"])
        if info is None:
            raise GenError(f"no layout for {p.elem['name']}")
        return info[0]
    return type_size(p.elem)


def win_count_limits(em, mm, sdkver):
    """Clamps every count the callee receives to the buffer this process allocated for
    it, so no sizing rule, right or wrong, lets the callee write past a buffer."""
    out = []
    by_name = {q.name: q for q in mm.params}
    for p in mm.params:
        if p.kind != "array" or p.count is None:
            continue
        q = by_name.get(p.count)
        if q is None:
            raise GenError(f"{mm.full}: {p.name} is sized by {p.count}, which is no parameter")
        unit = 1 if p.count_bytes else win_elem_size(em, sdkver, p)
        capacity = f"bridge::capacity_of(a_{p.name})"
        if p.count_ptr:
            if q.kind != "array" or not is_int(q.elem):
                raise GenError(f"{mm.full}: count pointer {q.name} is no integer")
            out.append(f"    bridge::limit_at<{c_type(q.elem)}>(a_{q.name}, {capacity}, {unit});")
        else:
            if q.kind != "scalar" or not is_int(q.type):
                raise GenError(f"{mm.full}: count {q.name} is no integer")
            out.append(f"    bridge::limit(a_{q.name}, {capacity}, {unit});")
    return out


class GenError(Exception):
    pass


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


# --- driver ------------------------------------------------------------------------------

def choose_interfaces(parsed, only):
    """Each interface version with the newest SDK defining it, oldest SDK first so the
    newest overwrites, as Proton does."""
    chosen = {}
    for sdkver in reversed(SDK_VERSIONS):
        if sdkver not in parsed:
            continue
        for version, klass in parsed[sdkver]["mac"]["classes"].items():
            chosen[version] = (sdkver, klass)
    alias_to_target = {alias: target for target, aliases in VERSION_ALIASES.items() for alias in aliases}
    if only:
        wanted = set()
        missing = []
        for v in only:
            target = alias_to_target.get(v, v)
            if target in chosen:
                wanted.add(target)
            else:
                missing.append(v)
        chosen = {v: c for v, c in chosen.items() if v in wanted}
        return chosen, missing
    return chosen, []


def annotate_pointers(parsed):
    """Marks every top-level record on both targets with whether it holds pointers, so
    classification can refuse records the other process cannot read."""
    for sdk in parsed.values():
        for target in sdk.values():
            for rec in target["records"].values():
                rec["_has_pointers"] = record_has_pointers(rec)


def resolve_record_flags(parsed, sdkver, t):
    """Copies the pointer flag of a named record into a type descriptor (recursively for
    arrays), since method signatures refer to records by name."""
    k = t["k"]
    if k == "record" and "_has_pointers" not in t:
        rec = parsed[sdkver]["mac"]["records"].get(t["name"])
        t["_has_pointers"] = rec["_has_pointers"] if rec else True
    elif k in ("ptr", "ref"):
        resolve_record_flags(parsed, sdkver, t["pointee"])
    elif k == "array":
        resolve_record_flags(parsed, sdkver, t["elem"])


def msvc_slots(methods):
    """Vtable slot of every method under MSVC: overloads grouped at the first
    declaration, in reverse declaration order."""
    order = sorted(methods, key=lambda m: (m["index"], -m["override"]))
    return {m["name"]: i for i, m in enumerate(order)}


def build_callbacks(parsed, sdkvers):
    """(id, win size, mac size, runs) for every callback struct, newest SDK first, one
    entry per distinct (id, sizes)."""
    seen = set()
    defs = []
    for sdkver in sdkvers:
        if sdkver not in parsed:
            continue
        mac_records = parsed[sdkver]["mac"]["records"]
        win_records = parsed[sdkver]["win"]["records"]
        for name, mac in mac_records.items():
            cid = mac.get("callback_id")
            if cid is None or name not in win_records:
                continue
            win = win_records[name]
            runs = conversion_runs(mac, win)
            if runs is None:
                continue
            key = (cid, win["size"], mac["size"])
            if key in seen:
                continue
            seen.add(key)
            defs.append((cid, win["size"], mac["size"], runs, name, sdkver))
    defs.sort(key=lambda d: (d[0], -d[1]))
    return defs


def protocol_hash(method_names, signatures, callbacks):
    h = hashlib.sha256()
    for name in method_names:
        h.update(name.encode())
        h.update(b"\0")
        h.update(signatures[name].encode())
        h.update(b"\n")
    for cid, w, m, runs, name, sdkver in callbacks:
        h.update(f"{cid}:{w}:{m}:{runs}\n".encode())
    return int.from_bytes(h.digest()[:8], "little")


def wire_signature(mm):
    parts = []
    for p in mm.params:
        parts.append(f"{p.kind}:{p.count or ''}:{int(bool(p.count_ptr))}:{int(bool(p.count_bytes))}:{p.fixed}:{int(p.inout)}")
    return f"{mm.ret}|" + ",".join(parts) + f"|{mm.slot}|{mm.special or ''}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma-separated interface versions")
    ap.add_argument("--cache", default=None, help="parsed-SDK JSON to reuse")
    ap.add_argument("--reparse", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--sdk-include", default=None,
                    help="include prefix for SDK headers in the generated code (default: the sdk root)")
    args = ap.parse_args()

    sdk_root = os.path.abspath(args.sdk_root)
    present = [v for v in SDK_VERSIONS if os.path.isdir(os.path.join(sdk_root, f"steamworks_sdk_{v}"))]
    if not present:
        sys.exit(f"no steamworks_sdk_* under {sdk_root}")

    parsed = None
    if args.cache and not args.reparse and os.path.exists(args.cache):
        with open(args.cache) as f:
            parsed = json.load(f)
        if sorted(parsed) != sorted(present):
            parsed = None
    if parsed is None:
        parsed = parse_all(sdk_root, present, args.jobs)
        if args.cache:
            with open(args.cache, "w") as f:
                json.dump(parsed, f)

    # The x86_64 macOS parse exists to prove the arm64 layouts hold there too.
    for sdkver, sdk in parsed.items():
        for name, rec in sdk["mac"]["records"].items():
            other = sdk["mac_x86"]["records"].get(name)
            if other is None:
                continue
            a, b = [], []
            leaves(rec, 0, a)
            leaves(other, 0, b)
            if a != b or rec["size"] != other["size"]:
                sys.exit(f"SDK {sdkver}: {name} differs between arm64 and x86_64 macOS")

    annotate_pointers(parsed)
    only = [v for v in args.only.split(",") if v] if args.only else None
    chosen, missing = choose_interfaces(parsed, only)

    sdk_rel = args.sdk_include or sdk_root
    em = Emitter(None, args.out, parsed, sdk_rel)

    interfaces = []
    for version in sorted(chosen):
        sdkver, klass = chosen[version]
        slots = msvc_slots(klass["methods"])
        methods = []
        for m in klass["methods"]:
            mm = MethodModel(klass, m, sdkver)
            for p in m["params"]:
                resolve_record_flags(parsed, sdkver, p["type"])
            resolve_record_flags(parsed, sdkver, m["result"])
            if not m["dtor"]:
                classify_method(mm)
            mm.slot = slots[m["name"]]
            methods.append(mm)
        interfaces.append({"klass": klass, "sdkver": sdkver, "methods": methods, "version": version})

    suspects = guessed_arrays(interfaces)
    if suspects:
        sys.exit("pointers sized as one element but named like arrays; give each a PARAM_OVERRIDES "
                 "decision (\"single\" when it really is one):\n  " + "\n  ".join(suspects))

    all_methods = [mm for iface in interfaces for mm in iface["methods"] if not mm.m["dtor"]]
    method_ids = {}
    next_id = 64
    for mm in all_methods:
        method_ids[mm.full] = next_id
        next_id += 1
    signatures = {mm.full: wire_signature(mm) for mm in all_methods}
    callbacks = build_callbacks(parsed, present)
    phash = protocol_hash([mm.full for mm in all_methods], signatures, callbacks)

    for iface in interfaces:
        sdkver = iface["sdkver"]
        includes = parsed[sdkver]["mac"]["includes"]
        emit_mac_class(em, iface, sdkver, includes, method_ids)
        emit_win_class(em, iface, sdkver)

    emit_tables(em, interfaces, all_methods, method_ids, callbacks, phash, chosen)
    emit_report(em, interfaces, missing, callbacks, phash)
    print(f"{len(interfaces)} interfaces, {len(all_methods)} methods, {len(callbacks)} callback layouts, "
          f"protocol {phash:016x}", file=sys.stderr)


def emit_tables(em, interfaces, all_methods, method_ids, callbacks, phash, chosen):
    # mac: interface factories, method names, callback table, protocol hash
    lines = ["// Generated by gen_bridge.py; do not edit.", '#include "proxy_prelude.h"', ""]
    for iface in interfaces:
        full = f"{iface['klass']['name']}_{iface['version']}"
        lines.append(f"void *bridge_create_{full}(uint64_t);")
    lines.append("")
    lines.append("const bridge::InterfaceDef bridge_interfaces[] = {")
    for iface in interfaces:
        full = f"{iface['klass']['name']}_{iface['version']}"
        lines.append(f'    {{"{iface["version"]}", &bridge_create_{full}}},')
        for alias in VERSION_ALIASES.get(iface["version"], []):
            lines.append(f'    {{"{alias}", &bridge_create_{full}}},')
    lines.append("};")
    lines.append("const size_t bridge_interface_count = sizeof(bridge_interfaces) / sizeof(bridge_interfaces[0]);")
    lines.append("")
    lines.append("const char *const bridge_method_names[] = {")
    for mm in all_methods:
        lines.append(f'    "{mm.full}",')
    lines.append("};")
    lines.append("const size_t bridge_method_count = sizeof(bridge_method_names) / sizeof(bridge_method_names[0]);")
    lines.append(f"const uint64_t bridge_protocol_hash = 0x{phash:016x}ull;")
    lines.append("")
    for i, (cid, w, m, runs, name, sdkver) in enumerate(callbacks):
        if runs:
            body = ", ".join(f"{{{a}, {b}, {n}}}" for a, b, n in runs)
            lines.append(f"static const bridge::Run cb_runs_{i}[] = {{ {body} }};")
    lines.append("const bridge::CallbackDef bridge_callbacks[] = {")
    for i, (cid, w, m, runs, name, sdkver) in enumerate(callbacks):
        r = f"cb_runs_{i}, {len(runs)}" if runs else "nullptr, 0"
        lines.append(f"    {{{cid}, {w}, {m}, {r}}},  // {name} ({sdkver})")
    lines.append("};")
    lines.append("const size_t bridge_callback_count = sizeof(bridge_callbacks) / sizeof(bridge_callbacks[0]);")
    write_if_changed(os.path.join(em.out, "mac", "tables.cpp"), "\n".join(lines) + "\n")

    # win: handler table by method id, method names, callback sizes, protocol hash
    lines = ["// Generated by gen_bridge.py; do not edit.", '#include "dispatch_prelude.h"', ""]
    for mm in all_methods:
        if mm.local or mm.unsupported:
            continue
        lines.append(f"void bridge_h_{mm.full}(bridge::Req &, bridge::Rep &);")
    lines.append("")
    lines.append("const bridge::Handler bridge_handlers[] = {")
    for mm in all_methods:
        if mm.local or mm.unsupported:
            lines.append(f"    nullptr,  // {mm.full}")
        else:
            lines.append(f"    &bridge_h_{mm.full},")
    lines.append("};")
    lines.append("const size_t bridge_handler_count = sizeof(bridge_handlers) / sizeof(bridge_handlers[0]);")
    lines.append("const char *const bridge_method_names[] = {")
    for mm in all_methods:
        lines.append(f'    "{mm.full}",')
    lines.append("};")
    lines.append(f"const uint64_t bridge_protocol_hash = 0x{phash:016x}ull;")
    lines.append("const bridge::CallbackDef bridge_callbacks[] = {")
    for cid, w, m, runs, name, sdkver in callbacks:
        lines.append(f"    {{{cid}, {w}, {m}, nullptr, 0}},  // {name} ({sdkver})")
    lines.append("};")
    lines.append("const size_t bridge_callback_count = sizeof(bridge_callbacks) / sizeof(bridge_callbacks[0]);")
    write_if_changed(os.path.join(em.out, "win", "tables.cpp"), "\n".join(lines) + "\n")

    # the lists the build reads
    write_if_changed(os.path.join(em.out, "mac", "sources.txt"),
                     "".join(f"{iface['klass']['name']}_{iface['version']}.cpp\n" for iface in interfaces) + "tables.cpp\n")
    write_if_changed(os.path.join(em.out, "win", "sources.txt"),
                     "".join(f"{iface['klass']['name']}_{iface['version']}.cpp\n" for iface in interfaces) + "tables.cpp\n")


def emit_report(em, interfaces, missing, callbacks, phash):
    lines = ["# Steam bridge generation report", "",
             f"Protocol hash `{phash:016x}`. {len(interfaces)} interface versions, "
             f"{len(callbacks)} callback layouts.", ""]
    if missing:
        lines.append("## Interface versions requested but defined by no SDK")
        lines.append("")
        for v in missing:
            lines.append(f"- `{v}`")
        lines.append("")
    lines.append("## Unsupported and locally answered methods")
    lines.append("")
    for iface in interfaces:
        rows = [mm for mm in iface["methods"] if not mm.m["dtor"] and (mm.unsupported or mm.local)]
        if not rows:
            continue
        lines.append(f"### {iface['version']} (SDK {iface['sdkver']})")
        lines.append("")
        for mm in rows:
            what = "local" if mm.local else mm.unsupported
            lines.append(f"- `{mm.name}`: {what}")
        lines.append("")
    lines.append("## Sized pointer parameters")
    lines.append("")
    lines.append("How every pointer argument is sized; check the ones for interfaces a game uses.")
    lines.append("")
    for iface in interfaces:
        rows = []
        for mm in iface["methods"]:
            if mm.m["dtor"] or mm.unsupported or mm.local:
                continue
            for p in mm.params:
                if p.kind == "array":
                    if p.fixed is not None:
                        how = "single" if p.fixed == 1 else f"fixed[{p.fixed}]"
                    else:
                        how = f"{'bytes' if p.count_bytes else 'elements'} = {'*' if p.count_ptr else ''}{p.count}"
                    io = "in/out" if p.inout else "in"
                    rows.append(f"- `{mm.name}` `{p.name}` ({p.type['spell']}): {how}, {io}")
                elif p.kind == "outstr":
                    rows.append(f"- `{mm.name}` `{p.name}`: out string pointer")
        if rows:
            lines.append(f"### {iface['version']}")
            lines.append("")
            lines.extend(rows)
            lines.append("")
    write_if_changed(os.path.join(em.out, "REPORT.md"), "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
