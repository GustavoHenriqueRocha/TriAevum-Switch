"""Onde ficam os arquivos do patch PT-BR (nada disso vai para o repositório: são dados do jogo e do mod).

TRIAEVUM_PTBR      pasta de trabalho (padrão ~/oot3d-work/ptbr): eu-original.qm, traducao.json, glossario.json,
                   fontes/ (M PLUS Rounded 1c, OFL), menus-pt/, romfs-ptbr.bin
TRIAEVUM_ROMFS     romfs.bin ORIGINAL do jogo (só leitura)
TRIAEVUM_SOH_TEXTO recurso text/nes_message_data_static extraído do mod PT-BR do Ship of Harkinian (otr.py extrai)
"""
import os
from pathlib import Path

TRABALHO = Path(os.environ.get('TRIAEVUM_PTBR', Path.home() / 'oot3d-work/ptbr'))
ROMFS_ORIGINAL = Path(os.environ.get('TRIAEVUM_ROMFS', Path.home() / 'oot3d-work/sd/switch/oot3dre/game/romfs.bin'))
SOH_TEXTO = Path(os.environ.get('TRIAEVUM_SOH_TEXTO', Path.home() / 'oot3d-work/soh-ptbr/extraido/'
                                'TraducaoPTBR-SolidOne-LordeJabiraca/text/nes_message_data_static/'
                                'nes_message_data_static'))
QM_ORIGINAL = TRABALHO / 'eu-original.qm'
ROMFS_PTBR = TRABALHO / 'romfs-ptbr.bin'
FONTES = TRABALHO / 'fontes'
