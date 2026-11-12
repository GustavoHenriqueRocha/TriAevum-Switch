"""Where the PT-BR patch files live (none of this goes into the repository: it is game and mod data).

TRIAEVUM_PTBR      working folder (default ~/oot3d-work/ptbr): eu-original.qm, translation.json, glossary.json,
                   fonts/ (M PLUS Rounded 1c, OFL), menus-pt/, romfs-ptbr.bin
TRIAEVUM_ROMFS     romfs.bin the game's ORIGINAL (read-only)
TRIAEVUM_SOH_TEXT  text/nes_message_data_static resource extracted from the Ship of Harkinian PT-BR mod (otr.py extracts it)
"""
import os
from pathlib import Path

WORK_DIR = Path(os.environ.get('TRIAEVUM_PTBR', Path.home() / 'oot3d-work/ptbr'))
ROMFS_ORIGINAL = Path(os.environ.get('TRIAEVUM_ROMFS', Path.home() / 'oot3d-work/sd/switch/oot3dre/game/romfs.bin'))
SOH_TEXT = Path(os.environ.get('TRIAEVUM_SOH_TEXT', Path.home() / 'oot3d-work/soh-ptbr/extracted/'
                                'TraducaoPTBR-SolidOne-LordeJabiraca/text/nes_message_data_static/'
                                'nes_message_data_static'))
QM_ORIGINAL = WORK_DIR / 'eu-original.qm'
ROMFS_PTBR = WORK_DIR / 'romfs-ptbr.bin'
FONTS = WORK_DIR / 'fonts'
