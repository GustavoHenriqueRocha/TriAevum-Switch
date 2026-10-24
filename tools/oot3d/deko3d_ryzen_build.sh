#!/bin/bash
# build.sh [dry|build] : build do backend deko3d sem recompilar shards
A=/root/agentes/deko3d
MODE="${1:-build}"
ST=$A/status.txt
inner() {
  mount --bind $A/src /home/gustavo/projetos/TriAevum
  mount --bind $A/build /home/gustavo/oot3d-work/build-switch
  mount --bind $A/gen/cpp-lean /home/gustavo/oot3d-work/gen/cpp-lean
  export OOT3D_OPERATIONAL_oot3dExtractRoot=/home/gustavo/oot3d-work/extract
  cd /home/gustavo/oot3d-work/build-switch
  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
  ninja build.ninja > $A/cmake.log 2>&1 || { echo "erro cmake" > $ST; tail -30 $A/cmake.log >> $ST; return 1; }
  if [ "$MODE" = dry ]; then
    ninja -n oot3d_native_game_nro 2>&1 | tail -n +1 > $A/dry.txt; echo "passos=$(grep -c '^\[' $A/dry.txt) shards=$(grep -c 'shard_' $A/dry.txt)"
    grep -oE 'Building [^ ]+ object [^ ]+' $A/dry.txt | awk '{print $4}' | sed 's#.*/##' | head -40
    return
  fi
  echo compilando > $ST
  nice -n 10 ninja -j 3 oot3d_native_game_nro > $A/build.log 2>&1 &
  pid=$!
  while kill -0 $pid 2>/dev/null; do
    if grep -q 'whole_aot_shard_' $A/build.log; then kill $pid; wait $pid; echo "erro: shard recompilando, abortado" > $ST; grep 'whole_aot_shard_' $A/build.log | head -3 >> $ST; return 1; fi
    sleep 2
  done
  wait $pid; rc=$?
  if [ $rc -ne 0 ]; then echo "erro rc=$rc" > $ST; tail -60 $A/build.log >> $ST; return 1; fi
  nro=oot3d_native_game.nro
  cp -f "$nro" $A/out/ && md5sum $A/out/$(basename $nro) > $A/out/$(basename $nro).md5
  echo "ok $(cat $A/out/$(basename $nro).md5)" > $ST
}
export -f inner; export A MODE ST
# RAM da Ryzen é defeituosa: nunca compilar com o Eden aberto.
if /mnt/c/Windows/System32/tasklist.exe 2>/dev/null | grep -qi '^eden.exe'; then echo "erro: Eden aberto; build recusado" > $ST; exit 1; fi
# Nunca dois builds ao mesmo tempo (RAM com defeito; WSL limitado a 6 GB).
if pgrep -x ninja >/dev/null 2>&1; then echo "erro: outro ninja rodando; build recusado" > $ST; exit 1; fi
echo iniciado > $ST
unshare -m bash -c inner
