#!/usr/bin/env bash
# Backup dos saves do TriAevum no Switch para o PC (e, opcional, para o VPS).
#
#   scripts/switch/backup-saves.sh                  descobre o Switch (FTP do Sphaira), copia e manda pro VPS
#   scripts/switch/backup-saves.sh --no-remote      só no PC
#   scripts/switch/backup-saves.sh --host 192.168.3.54
#   scripts/switch/backup-saves.sh --source <pasta> lê uma pasta local com o layout do SD (teste)
#
# O que copia (caminhos relativos ao sdmc:/), para sdmc:/switch/oot3dre e cada sdmc:/switch/triaevum/<título>:
#   savedata/, savedata-backups/, config/, extdata/ (recursivo) e os *.json / *.ini da raiz do título.
#   Não copia game/, logs/, caches, .nro nem boot-status.txt.
#
# Snapshot: <dest>/<AAAAMMDD-HHMMSS>/ com os arquivos no mesmo caminho do SD, SHA256SUMS e manifest.txt.
# Só cria snapshot quando o SHA256SUMS difere do último; arquivo igual ao do snapshot anterior vira
# hardlink (não ocupa espaço de novo). Nunca apaga snapshot: passando de --warn-after, só avisa.
#
# Nunca usa o FTP com o jogo aberto: o FTP do Sphaira só existe com o Sphaira aberto (o jogo fecha o
# Sphaira). Se nenhum host responde, sai com 0 e "Switch indisponível".
#
# Códigos de saída: 0 ok / sem mudança / Switch indisponível; 1 erro na cópia local (nada gravado);
# 2 snapshot local ok mas o VPS falhou (tenta de novo na próxima rodada); 64 uso errado.
set -euo pipefail

usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; }

hosts=()
port=5000
source_dir=""
dest="$HOME/oot3d-work/saves-backup"
remote=1
remote_host="vps"
remote_dir="backups/oot3dre-save"
warn_after=200
max_depth=8

while (($#)); do
    case "$1" in
        --host) hosts+=("${2:?--host precisa de um IP}"); shift 2 ;;
        --port) port="${2:?}"; shift 2 ;;
        --source) source_dir="${2:?}"; shift 2 ;;
        --dest) dest="${2:?}"; shift 2 ;;
        --no-remote) remote=0; shift ;;
        --remote-host) remote_host="${2:?}"; shift 2 ;;
        --remote-dir) remote_dir="${2:?}"; shift 2 ;;
        --warn-after) warn_after="${2:?}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "argumento desconhecido: $1" >&2; usage >&2; exit 64 ;;
    esac
done
if ((${#hosts[@]} == 0)); then
    read -r -a hosts <<< "${SWITCH_HOSTS:-192.168.3.54 192.168.100.24}"
fi
remote_dir="${remote_dir#\~/}"
[[ "$warn_after" =~ ^[0-9]+$ ]] || { echo "--warn-after precisa ser número" >&2; exit 64; }
if [[ -n "$source_dir" && ! -d "$source_dir" ]]; then
    echo "--source não é pasta: $source_dir" >&2; exit 64
fi

mkdir -p "$dest"
exec 9> "$dest/.lock"
if ! flock -n 9; then
    echo "outro backup já está rodando em $dest; saindo"; exit 0
fi

work=""
cleanup() { [[ -n "$work" ]] && rm -rf "$work"; return 0; }
trap cleanup EXIT
trap 'exit 130' INT TERM

snapshot_re='^[0-9]{8}-[0-9]{6}(-[0-9]+)?$'
list_snapshots() { # em ordem cronológica
    find "$dest" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | grep -E "$snapshot_re" | sort -V || true
}

warn_count() {
    local count
    count="$(list_snapshots | wc -l)"
    if ((count > warn_after)); then
        echo "AVISO: $count snapshots em $dest (aviso a partir de $warn_after); nada foi apagado." \
             "Se quiser liberar espaço, revise e apague à mão."
    fi
}

# ---------------------------------------------------------------- VPS
remote_status=0
ssh_remote() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" "$@"; }

sync_remote() { # manda todo snapshot local que ainda não está no VPS, conferindo o SHA-256 lá
    ((remote)) || return 0
    local qdir existing snap sums_hash qsnap
    qdir="$(printf '%q' "$remote_dir")"
    if ! existing="$(ssh_remote "mkdir -p $qdir && ls -1 $qdir")"; then
        echo "ERRO: VPS ($remote_host) inacessível; os snapshots ficam só no PC e sobem na próxima rodada" >&2
        remote_status=2; return 0
    fi
    while read -r snap; do
        [[ -z "$snap" ]] && continue
        grep -qxF "$snap" <<< "$existing" && continue
        sums_hash="$(sha256sum "$dest/$snap/SHA256SUMS" | cut -d' ' -f1)"
        qsnap="$(printf '%q' "$snap")"
        if tar -C "$dest" -cf - "$snap" | ssh_remote "set -e; cd $qdir; test ! -e $qsnap
            rm -rf .recebendo-$qsnap; mkdir .recebendo-$qsnap
            tar -C .recebendo-$qsnap -xf -
            cd .recebendo-$qsnap/$qsnap
            echo '$sums_hash  SHA256SUMS' | sha256sum --strict -c --quiet -
            sha256sum --strict -c --quiet SHA256SUMS
            cd ../..; mv .recebendo-$qsnap/$qsnap $qsnap; rmdir .recebendo-$qsnap"; then
            echo "VPS: $snap copiado e conferido em $remote_host:$remote_dir/$snap"
        else
            echo "ERRO: falha ao copiar/conferir $snap no VPS ($remote_host); tenta de novo na próxima" >&2
            remote_status=2
        fi
    done < <(list_snapshots)
}

finish() { # código_local
    sync_remote
    warn_count
    local code="$1"
    ((code == 0)) && code=$remote_status
    exit "$code"
}

# ---------------------------------------------------------------- origem: FTP ou pasta local
host=""
ftp_url() { # caminho relativo ao sdmc:/
    local rel="$1"
    rel="${rel//%/%25}"; rel="${rel// /%20}"; rel="${rel//#/%23}"; rel="${rel//\?/%3F}"
    printf 'ftp://%s:%s/%s' "$host" "$port" "$rel"
}
ftp_alive() { curl -s --connect-timeout 3 --max-time 10 -o /dev/null "ftp://$1:$port/"; }

# list_dir <rel>: imprime "d|f<TAB>tamanho<TAB>nome"; retorna 3 se a pasta não existe, 1 se a origem caiu.
list_dir() {
    local rel="$1" out rc=0
    if [[ -n "$source_dir" ]]; then
        [[ -d "$source_dir/$rel" ]] || return 3
        find "$source_dir/$rel" -mindepth 1 -maxdepth 1 \( -type f -o -type d \) -printf '%y\t%s\t%f\n' \
            | awk -F'\t' '$1 == "d" { print "d\t0\t" $3; next } { print }'
        return 0
    fi
    out="$(curl -sS --connect-timeout 5 --max-time 60 "$(ftp_url "$rel/")" 2> /dev/null)" || rc=$?
    case "$rc" in
        0) ;;
        9|78) return 3 ;;  # CWD recusado / arquivo inexistente: a pasta não existe
        *) return 1 ;;
    esac
    awk '{
        sub(/\r$/, ""); type = substr($1, 1, 1); size = $5; name = $0
        for (i = 1; i <= 8; i++) sub(/^[^ ]+ +/, "", name)
        if (name == "." || name == ".." || name == "") next
        if (type == "d") print "d\t0\t" name
        else if (type == "-") print "f\t" size "\t" name
    }' <<< "$out"
}

fetch_file() { # <rel> <tamanho esperado>
    local rel="$1" size="$2" out="$work/snap/$1" got
    mkdir -p "$(dirname "$out")"
    if [[ -n "$source_dir" ]]; then
        cp --preserve=timestamps "$source_dir/$rel" "$out"
    else
        curl -sS --connect-timeout 5 --max-time 300 -o "$out" "$(ftp_url "$rel")" || return 1
        [[ -e "$out" ]] || : > "$out"  # o curl não cria o arquivo quando o FTP manda 0 bytes
    fi
    got="$(stat -c %s "$out")"
    if [[ "$got" != "$size" ]]; then
        echo "ERRO: $rel veio com $got bytes, a listagem dizia $size" >&2
        return 1
    fi
    printf '%s\t%s\n' "$size" "$rel" >> "$work/files.tsv"
}

collect_dir() { # <rel> <profundidade>
    local rel="$1" depth="$2" listing rc=0 type size name
    listing="$(list_dir "$rel")" || rc=$?
    ((rc == 3)) && return 0
    ((rc == 0)) || return 1
    while IFS=$'\t' read -r type size name; do
        [[ -z "$name" ]] && continue
        if [[ "$type" == d ]]; then
            if ((depth >= max_depth)); then
                echo "AVISO: pasta funda demais, não copiada: $rel/$name" >&2; continue
            fi
            collect_dir "$rel/$name" $((depth + 1)) || return 1
        else
            fetch_file "$rel/$name" "$size" || return 1
        fi
    done <<< "$listing"
}

collect_title() { # <rel da raiz do título>
    local root="$1" listing rc=0 type size name
    listing="$(list_dir "$root")" || rc=$?
    ((rc == 3)) && return 0
    ((rc == 0)) || return 1
    while IFS=$'\t' read -r type size name; do
        [[ -z "$name" ]] && continue
        if [[ "$type" == d ]]; then
            case "$name" in
                savedata|savedata-backups|config|extdata) collect_dir "$root/$name" 1 || return 1 ;;
            esac
        else
            case "$name" in
                *.json|*.ini) fetch_file "$root/$name" "$size" || return 1 ;;
            esac
        fi
    done <<< "$listing"
}

collect_all() {
    local listing rc=0 type size name
    collect_title "switch/oot3dre" || return 1
    listing="$(list_dir "switch/triaevum")" || rc=$?
    ((rc == 3)) && return 0
    ((rc == 0)) || return 1
    while IFS=$'\t' read -r type size name; do
        [[ "$type" == d && -n "$name" ]] || continue
        collect_title "switch/triaevum/$name" || return 1
    done <<< "$listing"
}

if [[ -n "$source_dir" ]]; then
    origin="local:$(cd "$source_dir" && pwd)"
else
    for candidate in "${hosts[@]}"; do
        if ftp_alive "$candidate"; then host="$candidate"; break; fi
    done
    if [[ -z "$host" ]]; then
        echo "Switch indisponível (FTP do Sphaira não respondeu em ${hosts[*]} porta $port); nada copiado"
        finish 0
    fi
    origin="ftp://$host:$port"
fi

work="$(mktemp -d "$dest/.trabalho-XXXXXX")"
mkdir "$work/snap"
: > "$work/files.tsv"
echo "copiando de $origin"
if ! collect_all; then
    if [[ -n "$host" ]] && ! ftp_alive "$host"; then
        echo "Switch indisponível (o FTP fechou durante a cópia); nenhum snapshot gravado"
        finish 0
    fi
    echo "ERRO: cópia incompleta de $origin; nenhum snapshot gravado" >&2
    finish 1
fi

count="$(wc -l < "$work/files.tsv")"
if ((count == 0)); then
    echo "ERRO: nenhum save encontrado em $origin (sdmc:/switch/oot3dre sumiu?); nenhum snapshot gravado" >&2
    finish 1
fi

(cd "$work/snap" && find . -type f -printf '%P\n' | LC_ALL=C sort | xargs -d '\n' -r sha256sum --) \
    > "$work/SHA256SUMS"

previous="$(list_snapshots | tail -n 1)"
if [[ -n "$previous" ]] && cmp -s "$work/SHA256SUMS" "$dest/$previous/SHA256SUMS"; then
    echo "sem mudança desde $previous ($count arquivos conferidos por SHA-256); nenhum snapshot novo"
    finish 0
fi

# Mudanças em relação ao snapshot anterior; arquivo igual vira hardlink do anterior.
: > "$work/changes"
if [[ -n "$previous" ]]; then
    LC_ALL=C awk 'NR == FNR { old[substr($0, 67)] = substr($0, 1, 64); next }
        { path = substr($0, 67); hash = substr($0, 1, 64); seen[path] = 1
          if (!(path in old)) print "novo\t" path
          else if (old[path] != hash) print "mudou\t" path
          else print "igual\t" path }
        END { for (path in old) if (!(path in seen)) print "sumiu\t" path }' \
        "$dest/$previous/SHA256SUMS" "$work/SHA256SUMS" | LC_ALL=C sort -k2 > "$work/changes"
    while IFS=$'\t' read -r kind path; do
        if [[ "$kind" == igual ]]; then
            ln -f "$dest/$previous/$path" "$work/snap/$path" 2> /dev/null || true
        elif [[ "$kind" == sumiu ]]; then
            echo "AVISO: sumiu do Switch: $path (continua guardado em $dest/$previous)"
        fi
    done < "$work/changes"
fi

stamp="$(date +%Y%m%d-%H%M%S)"
name="$stamp"; n=2
while [[ -e "$dest/$name" ]]; do name="$stamp-$n"; n=$((n + 1)); done

{
    echo "# backup dos saves do TriAevum (scripts/switch/backup-saves.sh)"
    echo "criado=$(date --iso-8601=seconds)"
    echo "origem=$origin"
    echo "maquina=$(hostname 2> /dev/null || cat /etc/hostname 2> /dev/null || echo -)"
    echo "script_commit=$(git -C "$(dirname "$0")" rev-parse --short HEAD 2> /dev/null || echo -)"
    echo "anterior=${previous:--}"
    echo "arquivos=$count"
    echo "bytes=$(awk -F'\t' '{ s += $1 } END { print s + 0 }' "$work/files.tsv")"
    echo "# mudanças em relação ao anterior (novo/mudou/sumiu)"
    if [[ -n "$previous" ]]; then
        grep -v '^igual' "$work/changes" || true
    else
        echo "primeiro snapshot"
    fi
    echo "# tamanho<TAB>caminho (relativo ao sdmc:/)"
    LC_ALL=C sort -t $'\t' -k2 "$work/files.tsv"
} > "$work/snap/manifest.txt"
cp "$work/SHA256SUMS" "$work/snap/SHA256SUMS"

if ! (cd "$work/snap" && sha256sum --strict -c --quiet SHA256SUMS); then
    echo "ERRO: SHA256SUMS não confere na pasta nova; nenhum snapshot gravado" >&2
    finish 1
fi
find "$work/snap" -type f -exec chmod a-w {} +
mv -T "$work/snap" "$dest/$name"
echo "snapshot novo: $dest/$name ($count arquivos)"
if [[ -n "$previous" ]]; then
    grep -v '^igual' "$work/changes" | sed 's/^/  /' || true
fi
finish 0
