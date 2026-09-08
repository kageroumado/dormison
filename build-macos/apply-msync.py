#!/usr/bin/env python3
"""Apply CrossOver 26.3's msync sync backend to this wine tree.

Copies the four msync source files from msync-src in verbatim and rewrites the
hook sites in eight files. Every anchor is an exact string from the tree; a miss
is a hard error, because a half-applied sync backend builds and then deadlocks.
An edit whose result is already in the file is skipped.
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)          # the repository root is the wine tree
MSYNC = os.path.join(HERE, "msync-src")

def edit(path, old, new, count=1):
    p = os.path.join(SRC, path)
    s = open(p).read()
    if new in s:
        print(f"  = {path}: already applied"); return
    n = s.count(old)
    if n != count:
        sys.exit(f"ANCHOR MISS in {path}: expected {count} occurrence(s), found {n}\n---\n{old}\n---")
    open(p, "w").write(s.replace(old, new, count))
    print(f"  + {path}")

print("==> copying msync sources")
for rel in ("server/msync.c", "server/msync.h",
            "dlls/ntdll/unix/msync.c", "dlls/ntdll/unix/msync.h"):
    shutil.copy(os.path.join(MSYNC, rel), os.path.join(SRC, rel))
    print(f"  + {rel}")

print("==> build files")
edit("server/Makefile.in", "\tfile.c \\\n", "\tfile.c \\\n\tmsync.c \\\n")
edit("dlls/ntdll/Makefile.in", "\tunix/file.c \\\n", "\tunix/file.c \\\n\tunix/msync.c \\\n")

print("==> protocol")
edit("server/protocol.def",
     "    int           type;         /* inproc sync type */\n"
     "    unsigned int access;        /* handle access rights */\n@END",
     "    int           type;         /* inproc sync type */\n"
     "    unsigned int access;        /* handle access rights */\n"
     "    unsigned int shm_idx;       /* index into the msync shared block, when in use */\n@END")

print("==> server/main.c")
edit("server/main.c", '#include "unicode.h"\n', '#include "unicode.h"\n#include "msync.h"\n')
edit("server/main.c",
     "    sock_init();\n    open_master_socket();\n",
     "    sock_init();\n    msync_init_shm();\n    open_master_socket();\n    msync_init();\n")

print("==> server/thread.c")
edit("server/thread.c", '#include "security.h"\n', '#include "security.h"\n#include "msync.h"\n')
edit("server/thread.c",
     "        reply->handle = get_thread_id( current ) | 1; /* arbitrary token */\n"
     "        send_client_fd( current->process, fd, reply->handle );",
     "        if (do_msync())\n"
     "        {\n"
     "            reply->handle = (unsigned int)fd;\n"
     "            return;\n"
     "        }\n\n"
     "        reply->handle = get_thread_id( current ) | 1; /* arbitrary token */\n"
     "        send_client_fd( current->process, fd, reply->handle );")

print("==> server/inproc_sync.c")
edit("server/inproc_sync.c", '#include "user.h"\n', '#include "user.h"\n#include "msync.h"\n')
edit("server/inproc_sync.c",
     "    else send_client_fd( current->process, fd, req->handle );",
     "    else\n"
     "    {\n"
     "        if (do_msync()) reply->shm_idx = (unsigned int)fd;\n"
     "        else send_client_fd( current->process, fd, req->handle );\n"
     "    }")
edit("server/inproc_sync.c", "#else /* NTSYNC_IOC_EVENT_READ */",
     open(os.path.join(MSYNC, "inproc_sync_apple.c")).read() + "#else /* NTSYNC_IOC_EVENT_READ */")

print("==> dlls/ntdll/unix/sync.c")
edit("dlls/ntdll/unix/sync.c", '#include "unix_private.h"\n',
     '#include "unix_private.h"\n#include "msync.h"\n')
edit("dlls/ntdll/unix/sync.c", "#else /* NTSYNC_IOC_EVENT_READ */",
     open(os.path.join(MSYNC, "sync_apple.c")).read() + "#else /* NTSYNC_IOC_EVENT_READ */")
edit("dlls/ntdll/unix/sync.c",
     "    assert( ref >= 0 );\n    if (!ref) close( fd );",
     "    assert( ref >= 0 );\n"
     "    if (!ref)\n"
     "    {\n"
     "        if (do_msync()) msync_close( fd );\n"
     "        else close( fd );\n"
     "    }")
edit("dlls/ntdll/unix/sync.c",
     "            sync->fd = wine_server_receive_fd( &fd_handle );\n"
     "            assert( wine_server_ptr_handle(fd_handle) == handle );",
     "            if (do_msync()) sync->fd = reply->shm_idx;\n"
     "            else\n"
     "            {\n"
     "                sync->fd = wine_server_receive_fd( &fd_handle );\n"
     "                assert( wine_server_ptr_handle(fd_handle) == handle );\n"
     "            }")
edit("dlls/ntdll/unix/sync.c",
     "                data->alert_fd = fd = wine_server_receive_fd( &token );\n"
     "                assert( token == reply->handle );",
     "                if (do_msync()) data->alert_fd = fd = reply->handle;\n"
     "                else\n"
     "                {\n"
     "                    data->alert_fd = fd = wine_server_receive_fd( &token );\n"
     "                    assert( token == reply->handle );\n"
     "                }")

print("==> dlls/ntdll/unix/loader.c")
edit("dlls/ntdll/unix/loader.c", '#include "unix_private.h"\n',
     '#include "unix_private.h"\n#include "msync.h"\n')
edit("dlls/ntdll/unix/loader.c",
     "    startup_info_size = server_init_process();\n",
     "    startup_info_size = server_init_process();\n    msync_init();\n")

print("\nAll hooks applied. Regenerate the protocol with tools/make_requests, then rebuild.")
