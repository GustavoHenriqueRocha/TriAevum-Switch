"""Onde ficam os arquivos do patch PT-BR (nada disso vai para o repositório: são dados do jogo e do mod).

TRIAEVUM_PTBR      pasta de trabalho (padrão ~/oot3d-work/ptbr): eu-original.qm, traducao.json, glossario.json,
                   fontes/ (M PLUS Rounded 1c, OFL), menus-pt/, romfs-ptbr.bin
TRIAEVUM_ROMFS     romfs.bin ORIGINAL do jogo (só leitura)
TRIAEVUM_SOH_TEXTO recurso text/nes_message_data_static extraído do mod PT-BR do Ship of Harkinian (otr.py extrai)

Os padrões mudam por jogo (pastas(jogo)): o OoT3D usa os de cima; o MM3D (mm3d.py) usa ~/oot3d-work/conv/mm3d-ptbr e a
romfs extraída lá. As duas variáveis de ambiente valem para o jogo em uso.
"""
import os
from pathlib import Path

PADROES = {
    'oot3d': (Path.home() / 'oot3d-work/ptbr', Path.home() / 'oot3d-work/sd/switch/oot3dre/game/romfs.bin'),
    'mm3d': (Path.home() / 'oot3d-work/conv/mm3d-ptbr', Path.home() / 'oot3d-work/conv/mm3d-ptbr/romfs/romfs.bin'),
}


def pastas(jogo):
    """→ (pasta de trabalho, romfs original) do jogo."""
    trabalho, romfs = PADROES[jogo]
    return Path(os.environ.get('TRIAEVUM_PTBR', trabalho)), Path(os.environ.get('TRIAEVUM_ROMFS', romfs))


TRABALHO, ROMFS_ORIGINAL = pastas('oot3d')
SOH_TEXTO = Path(os.environ.get('TRIAEVUM_SOH_TEXTO', Path.home() / 'oot3d-work/soh-ptbr/extraido/'
                                'TraducaoPTBR-SolidOne-LordeJabiraca/text/nes_message_data_static/'
                                'nes_message_data_static'))
QM_ORIGINAL = TRABALHO / 'eu-original.qm'
ROMFS_PTBR = TRABALHO / 'romfs-ptbr.bin'
FONTES = TRABALHO / 'fontes'
