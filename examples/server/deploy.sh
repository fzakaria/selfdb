#!/usr/bin/env bash
# Deploy self-httpd to an ordinary Linux host over ssh.
#
# The host does not have to be NixOS. It needs a C compiler, libsqlite3-dev,
# python3, systemd and root, because three things have to be installed there:
#
#   /usr/local/bin/self-exec   the binfmt_misc interpreter
#   /etc/binfmt.d/self.conf    the registration that makes ./server runnable
#   /srv/self/server           the artifact: website, program and log, one file
#
# An existing deployment's `visits` and `presses` are carried over into the
# new build, because a deploy replaces the program but not the data -- and in
# this format both live in the same file.
#
# The deployment runs WAL by default: a public site should take the ~3x on
# response time, and the single-file property still holds at rest, because the
# unit stops the server with SIGTERM and every worker closes its connection.
# Pass `delete` as the third argument for one file even while serving.
#
# Usage: bash examples/server/deploy.sh [user@]host [port] [wal|delete]
set -euo pipefail

host="${1:?usage: deploy.sh [user@]host [port] [wal|delete]}"
port="${2:-8080}"
journal="${3:-wal}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
remote_repo="selfdb"
install_dir="/srv/self"
interpreter="/usr/local/bin/self-exec"
service="self-httpd"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# ── the tree, minus everything that is host-specific ─────────────────
say "copying $repo to $host:~/$remote_repo"
tar czf - -C "$repo" \
	--exclude=.git --exclude=.jj --exclude=__pycache__ \
	--exclude='*.o' --exclude='*.so' --exclude=loader/self-exec . |
	ssh "$host" "rm -rf ~/$remote_repo && mkdir -p ~/$remote_repo && tar xzf - -C ~/$remote_repo"

# ── build there: the ELF must link the host's own glibc and sqlite ───
say "building self-exec and the server on $host"
ssh "$host" bash -euo pipefail -s <<REMOTE_BUILD
export DEBIAN_FRONTEND=noninteractive
if ! pkg-config --exists sqlite3; then
	sudo apt-get update -qq
	sudo apt-get install -y -qq libsqlite3-dev build-essential pkg-config python3-venv
fi

# elf2self needs LIEF; a venv keeps it out of the system python.
if [ ! -x ~/selfvenv/bin/python ]; then
	python3 -m venv ~/selfvenv
	~/selfvenv/bin/pip install -q lief
fi

cd ~/$remote_repo
make -C loader >/dev/null
PATH="\$HOME/selfvenv/bin:\$PATH" bash examples/server/build.sh ~/$remote_repo/server.new
REMOTE_BUILD

# ── teach the kernel the format, then install and start ──────────────
say "registering binfmt_misc and installing to $install_dir"
ssh "$host" bash -euo pipefail -s <<REMOTE_INSTALL
sudo install -Dm755 ~/$remote_repo/loader/self-exec $interpreter

# binfmt_misc match: 'SQLite format 3\\0' at offset 0 with the 4-byte
# application_id at offset 68 equal to 'SELF'. Bytes 16..67 differ between
# databases and are masked out, so an ordinary SQLite file never matches.
zeros=\$(printf '\\\\x00%.0s' \$(seq 52))
mask_zeros=\$(printf '\\\\x00%.0s' \$(seq 52))
magic="\\\\x53\\\\x51\\\\x4c\\\\x69\\\\x74\\\\x65\\\\x20\\\\x66\\\\x6f\\\\x72\\\\x6d\\\\x61\\\\x74\\\\x20\\\\x33\\\\x00\${zeros}\\\\x53\\\\x45\\\\x4c\\\\x46"
mask="\$(printf '\\\\xff%.0s' \$(seq 16))\${mask_zeros}\\\\xff\\\\xff\\\\xff\\\\xff"
printf ':self:M:0:%s:%s:%s:\\n' "\$magic" "\$mask" "$interpreter" |
	sudo tee /etc/binfmt.d/self.conf >/dev/null
sudo systemctl restart systemd-binfmt.service
test -e /proc/sys/fs/binfmt_misc/self

sudo mkdir -p $install_dir
sudo chown "\$(id -u):\$(id -g)" $install_dir

# Stop first, then migrate. Under WAL the old artifact only becomes a
# self-contained single file once its last connection closes, so reading it
# while it still serves would race the visits it is still committing.
sudo systemctl stop $service.service 2>/dev/null || true

# A deploy replaces the program, not the data: carry the visitor log and the
# presses across from the old artifact into the new one.
if [ -f $install_dir/server ]; then
	sqlite3 ~/$remote_repo/server.new \
		"ATTACH '$install_dir/server' AS old;
		 INSERT INTO visits (at, ua, path) SELECT at, ua, path FROM old.visits;
		 INSERT INTO presses (at, button) SELECT at, button FROM old.presses;
		 DETACH old;"
fi

install -m755 ~/$remote_repo/server.new $install_dir/server

sudo tee /etc/systemd/system/$service.service >/dev/null <<UNIT
[Unit]
Description=self-httpd: a webserver that is a SQLite database
After=network.target systemd-binfmt.service
Requires=systemd-binfmt.service

[Service]
# No interpreter on the command line: binfmt_misc knows what this file is.
ExecStart=$install_dir/server --journal $journal $port
WorkingDirectory=$install_dir
Environment=SELF_MODE=memfd
User=\$(id -un)
Restart=always
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now $service.service
REMOTE_INSTALL

say "checking"
ssh "$host" "systemctl is-active $service.service; \
	curl -fsS -o /dev/null -w 'GET / -> %{http_code}\n' http://127.0.0.1:$port/; \
	sqlite3 $install_dir/server 'PRAGMA journal_mode' | sed 's/^/journal: /'; \
	ls $install_dir; \
	sqlite3 $install_dir/server 'SELECT count(*) FROM visits' | sed 's/^/visits: /'"

say "deployed. remember: ssh exe.dev share port <vm> $port"
