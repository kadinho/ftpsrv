#!/usr/bin/env python3
"""ferry.py — drive ps5-ferry's server-side commands from a PC.

The bytes never leave the console: the PC only sends control commands.

    ferry.py HOST df PATH                 free/total space of PATH's file system
    ferry.py HOST du PATH                 recursive size of PATH
    ferry.py HOST cp SRC DST [--detach]   server-side copy (resumable), shows progress
    ferry.py HOST jobs [ID]               list copy jobs
    ferry.py HOST cancel ID               stop a running copy
    ferry.py HOST rm PATH                 recursive delete (asks first)
    ferry.py HOST hash PATH               SHA-256 of a file, computed on the console

Typical move: `cp` -> test the game from DST -> `rm` the source.
"""
import argparse
import ftplib
import sys
import time

PORT = 2121


def fields(line):
    """Parse 'k=v;k=v' out of a reply line."""
    body = line[4:] if line[:3].isdigit() else line
    out = {}
    for part in body.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v
    return out


def human(n):
    n = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}"
        n /= 1024


def site(ftp, cmd):
    return ftp.sendcmd("SITE " + cmd)


def show_job(j):
    done, total = (int(x) for x in j["bytes"].split("/"))
    secs = max(int(j["secs"]), 1)
    print(f"#{j['job']} {j['state']:<9} {j['pct']:>3}%  "
          f"{human(done)}/{human(total)}  files {j['files']} "
          f"(skipped {j['skipped']})  {human(done / secs)}/s")
    print(f"    {j['src']} -> {j['dst']}")
    if j.get("current"):
        print(f"    now: {j['current']}")
    if j.get("error"):
        print(f"    error: {j['error']}")


def cmd_cp(ftp, args):
    free = fields(site(ftp, f"DF {args.dst.rsplit('/', 1)[0] or '/'}"))
    size = fields(site(ftp, f"DU {args.src}"))
    print(f"source {human(size['bytes'])} in {size['files']} files; "
          f"destination has {human(free['avail'])} free")

    site(ftp, f"CPFR {args.src}")
    reply = site(ftp, f"CPTO {args.dst}")
    job = reply.split()[3]
    print(reply)
    if args.detach:
        return 0

    while True:
        j = fields(site(ftp, f"JOBS {job}"))
        sys.stdout.write(f"\r{j['state']:<9} {j['pct']:>3}%  "
                         f"{human(j['bytes'].split('/')[0])}  "
                         f"files {j['files']}   ")
        sys.stdout.flush()
        if j["state"] not in ("scanning", "copying"):
            print()
            show_job(j)
            return 0 if j["state"] == "done" else 1
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("-p", "--port", type=int, default=PORT)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("df").add_argument("path")
    sub.add_parser("du").add_argument("path")
    cp = sub.add_parser("cp")
    cp.add_argument("src")
    cp.add_argument("dst")
    cp.add_argument("--detach", action="store_true",
                    help="start the job and return; poll with `jobs`")
    sub.add_parser("jobs").add_argument("id", nargs="?")
    sub.add_parser("cancel").add_argument("id")
    rm = sub.add_parser("rm")
    rm.add_argument("path")
    rm.add_argument("-y", "--yes", action="store_true")
    sub.add_parser("hash").add_argument("path")
    args = ap.parse_args()

    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=600)
    ftp.login()
    try:
        if args.cmd == "df":
            f = fields(site(ftp, f"DF {args.path}"))
            print(f"avail {human(f['avail'])}  free {human(f['free'])}  "
                  f"total {human(f['total'])}")
        elif args.cmd == "du":
            f = fields(site(ftp, f"DU {args.path}"))
            print(f"{human(f['bytes'])}  ({f['bytes']} bytes, {f['files']} files, "
                  f"{f['dirs']} dirs)")
        elif args.cmd == "cp":
            return cmd_cp(ftp, args)
        elif args.cmd == "jobs":
            reply = site(ftp, "JOBS" + (f" {args.id}" if args.id else ""))
            for line in reply.splitlines():
                if "job=" in line:
                    show_job(fields(line))
                else:
                    print(line)
        elif args.cmd == "cancel":
            print(site(ftp, f"CANCEL {args.id}"))
        elif args.cmd == "rm":
            f = fields(site(ftp, f"DU {args.path}"))
            print(f"{args.path}: {human(f['bytes'])}, {f['files']} files")
            if not args.yes and input("delete it? [y/N] ").lower() != "y":
                return 1
            print(site(ftp, f"RMTREE {args.path}"))
        elif args.cmd == "hash":
            print(site(ftp, f"HASH {args.path}"))
    except ftplib.error_perm as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        try:
            ftp.quit()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
