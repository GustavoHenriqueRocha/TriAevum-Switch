#!/usr/bin/env bash
# Launcher do ccache para os shards whole-AOT compilados com PGO do GCC.
#
# Sem ele o ccache (4.14) nunca guardava esses shards e todo build "use"
# recompilava os 256:
# - recusa -fprofile-partial-training ("unsupported compiler option");
# - com -fprofile-prefix-path procura o .gcda com o nome errado, não acha e
#   desiste ("no profile data file found").
# Aqui as flags de PGO saem da linha que o ccache vê e voltam num wrapper do
# compilador. O conteúdo do wrapper (flags + versão do GCC real) é o que o
# ccache compara (compiler_check=content), e o .gcda do objeto entra no hash
# (CCACHE_EXTRAFILES), então um perfil novo invalida só os shards cujo .gcda
# mudou.
#
#   ccache-pgo.sh <diretório dos .gcda> <compilador> <argumentos...>
# O diretório de trabalho é a árvore de build (-fprofile-prefix-path), como no
# Ninja; o .gcda se chama <objeto sem extensão, com / trocado por #>.gcda.
set -eu
profile_dir=$1 compiler=$2
shift 2

args=() pgo=() object= previous=
for arg in "$@"; do
    case $arg in
        -fprofile-use=* | -fprofile-prefix-path=* | -fprofile-partial-training) pgo+=("$arg") ;;
        *) args+=("$arg") ;;
    esac
    [ "$previous" = -o ] && object=$arg
    previous=$arg
done
[ ${#pgo[@]} -gt 0 ] || exec ccache "$compiler" "${args[@]}"

object=${object#"$PWD"/}
gcda="$profile_dir/$(printf '%s' "${object%.*}" | tr / '#').gcda"
[ -f "$gcda" ] && export CCACHE_EXTRAFILES=$gcda

flags=$(printf ' %q' "$compiler" "${pgo[@]}")
key=$(printf '%s' "$flags" | sha256sum | cut -c1-16)
wrapper="$PWD/ccache-pgo/$key/$(basename "$compiler")"
if [ ! -x "$wrapper" ]; then
    mkdir -p "${wrapper%/*}"
    tmp=$(mktemp "${wrapper%/*}/.wrapper.XXXXXX")
    {
        printf '#!/bin/sh\n# %s\n' "$("$compiler" --version | head -1)"
        printf 'exec%s "$@"\n' "$flags"
    } > "$tmp"
    chmod +x "$tmp"
    mv -f "$tmp" "$wrapper"
fi
export CCACHE_COMPILERCHECK=content
exec ccache "$wrapper" "${args[@]}"
