#!/bin/bash
# Lanca uma versao nova do M5tabOS. As regras de qual numero subir estao em
# VERSIONING.md.
#
#   tools/release.sh patch     0.7.0 -> 0.7.1  (so correcoes)
#   tools/release.sh minor     0.7.0 -> 0.8.0  (algo novo, ou que exige regravar)
#   tools/release.sh major     0.7.0 -> 1.0.0
#   tools/release.sh 0.9.0     um numero escolhido (tem que ser maior)
#
# O que faz, nesta ordem, e so depois de voce confirmar:
#   1. version.txt recebe o numero novo;
#   2. no CHANGELOG.md, o que esta em "Nao lancado" vira a secao da versao,
#      com a data de hoje, e um "Nao lancado" vazio volta para o topo;
#   3. commit "Versao X.Y.Z" e tag anotada vX.Y.Z.
# Nao envia nada: o push fica com voce (o comando sai impresso no fim).
#
# Recusa quando: ha mudanca nao commitada, o numero nao e maior que a ultima
# versao lancada, ou o "Nao lancado" do CHANGELOG esta vazio - versao sem
# nada escrito sobre ela e versao que ninguem sabe o que trouxe.
set -euo pipefail
cd "$(dirname "$0")/.."

falha() { echo "lancar: $*" >&2; exit 1; }

[ $# -eq 1 ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

git diff --quiet && git diff --cached --quiet || falha "ha mudancas nao commitadas - commite ou guarde antes de lancar."

atual=$(tr -d ' \r\n' < version.txt)
[[ "$atual" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || falha "version.txt nao tem X.Y.Z: '$atual'"
ultima=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null | sed 's/^v//' || true)

# Base da conta: o maior entre o version.txt e a ultima tag.
base="$atual"
if [ -n "$ultima" ] && [ "$(printf '%s\n%s\n' "$ultima" "$atual" | sort -V | tail -1)" = "$ultima" ]; then
  base="$ultima"
fi
IFS=. read -r MA MI PA <<< "$base"

case "$1" in
  patch) nova="$MA.$MI.$((PA + 1))" ;;
  minor) nova="$MA.$((MI + 1)).0" ;;
  major) nova="$((MA + 1)).0.0" ;;
  *)     nova="$1" ;;
esac
[[ "$nova" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || falha "versao invalida: '$nova' (use patch, minor, major ou X.Y.Z)"

if [ -n "$ultima" ]; then
  [ "$nova" != "$ultima" ] && [ "$(printf '%s\n%s\n' "$ultima" "$nova" | sort -V | tail -1)" = "$nova" ] \
    || falha "$nova nao e maior que a ultima versao lancada ($ultima)."
fi
git rev-parse -q --verify "refs/tags/v$nova" >/dev/null && falha "a tag v$nova ja existe."

# O "Nao lancado" precisa ter alguma linha de conteudo antes da proxima secao.
# ([^]]* e nao [aã]: o awk do macOS le byte a byte, e "ã" sao dois bytes.)
conteudo=$(awk '/^## \[N[^]]*o lan[^]]*ado\]/{dentro=1; next} /^## \[/{dentro=0} dentro && NF' CHANGELOG.md | wc -l | tr -d ' ')
[ "$conteudo" -gt 0 ] || falha "o \"Nao lancado\" do CHANGELOG.md esta vazio - escreva o que esta versao traz."

ramo=$(git rev-parse --abbrev-ref HEAD)
commits=$(git rev-list --count "${ultima:+v$ultima..}HEAD")
echo "M5tabOS: ${ultima:-nenhuma} -> $nova"
echo "  ramo $ramo, $commits commits desde a ultima versao, $conteudo linhas no CHANGELOG"
[ "$ramo" = "main" ] || echo "  ATENCAO: versao fora do main."
read -r -p "Confirma? [s/N] " ok
[[ "$ok" =~ ^[sS]$ ]] || falha "cancelado."

hoje=$(date +%Y-%m-%d)
printf '%s\n' "$nova" > version.txt
awk -v v="$nova" -v d="$hoje" '
  !feito && /^## \[N[^]]*o lan[^]]*ado\]/ { print; print ""; print "## [" v "] - " d; feito=1; next }
  { print }' CHANGELOG.md > CHANGELOG.md.novo
mv CHANGELOG.md.novo CHANGELOG.md

git add version.txt CHANGELOG.md
git commit -q -m "Versao $nova"
git tag -a "v$nova" -m "M5tabOS $nova"

echo "Pronto: commit e tag v$nova criados. Para publicar:"
echo "  git push origin $ramo v$nova"
